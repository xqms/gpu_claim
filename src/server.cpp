// Server
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include <nvml.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <pwd.h>

#include <zpp_bits.h>

#include "protocol.h"

std::vector<Card> g_cards;
std::size_t gpuLimitPerUser = 2;

namespace
{
    template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
}

void claim(Card& card, int uid)
{
    if(uid < 0)
        throw std::logic_error{"claim(): Invalid UID"};

    char buf[256];
    snprintf(buf, sizeof(buf), "/dev/nvidia%u", card.minorID);

    if(chown(buf, uid, -1) != 0)
    {
        fprintf(stderr, "Could not set owner of %s to UID %d: %s\n",
            buf, uid, strerror(errno)
        );
        std::exit(1);
    }
    card.reservedByUID = uid;
}

void release(Card& card)
{
    claim(card, 0);
}

struct Client
{
    int fd = -1;
    std::chrono::steady_clock::time_point connectTime;
    int uid = -1;

    explicit Client(int fd)
     : fd(fd)
     , connectTime{std::chrono::steady_clock::now()}
    {
        ucred cred{};
        socklen_t len = sizeof(cred);
        if(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        {
            perror("Could not get SO_PEERCRED option");
            return;
        }

        uid = cred.uid;
    }

    ~Client()
    {
        if(fd >= 0)
            close(fd);
    };

    // Return false if the client should be deleted
    [[nodiscard]] bool communicate()
    {
        // If authentication failed (see above), don't accept any commands.
        if(uid < 0)
            return false;

        std::array<uint8_t, 512> buf;

        int ret = read(fd, buf.data(), buf.size());
        if(ret == 0)
        {
            // Client has closed connection
            return false;
        }
        if(ret < 0)
        {
            perror("Could not read from client");
            return false;
        }

        printf("Got data from client!\n");

        zpp::bits::in in{buf};

        Request req;
        auto res = in(req);
        if(zpp::bits::failure(res))
        {
            fprintf(stderr, "Client sent request that could not be parsed\n");
            return false;
        }

        auto send = [&](auto& msg){
            auto [data, out] = zpp::bits::data_out();
            out(msg).or_throw();

            if(::send(fd, data.data(), data.size(), MSG_EOR) != data.size())
                perror("Could not send response");
        };

        return std::visit(overloaded {
            [&](const StatusRequest&) {
                printf("Status req\n");
                StatusResponse resp;
                resp.cards = g_cards;

                send(resp);
                return false;
            },
            [&](const ClaimRequest& req) {
                // Never allow someone to claim all cards
                int alreadyClaimed = std::ranges::count_if(g_cards, [=](auto& card) { return card.reservedByUID == uid; });
                if(alreadyClaimed + req.numGPUs > gpuLimitPerUser)
                {
                    ClaimResponse resp;
                    resp.error = "GPU per-user limit is reached";
                    send(resp);
                    return false;
                }

                // Can we immediately satisfy this request?
                std::vector<int> freeCards;
                for(std::size_t i = 0; i < g_cards.size(); ++i)
                {
                    auto& card = g_cards[i];
                    if(card.reservedByUID == 0)
                        freeCards.push_back(i);
                }

                if(req.numGPUs <= freeCards.size())
                {
                    ClaimResponse resp;
                    for(unsigned int i = 0; i < req.numGPUs; ++i)
                    {
                        claim(g_cards[freeCards[i]], uid);
                        resp.claimedCards.push_back(g_cards[freeCards[i]]);
                    }
                    send(resp);
                }
                else
                {
                    ClaimResponse resp;
                    resp.error = "Waiting is not implemented yet";
                    send(resp);
                }

                return false;
            },
            [&](auto) {
                fprintf(stderr, "Unhandled command type\n");
                return false;
            },
        }, req.data);
    }
};
std::vector<std::unique_ptr<Client>> g_clients;

void updateCardFromNVML(unsigned int devIdx, Card& card, const std::chrono::steady_clock::time_point& now)
{
    char buf[1024];
    std::array<nvmlProcessInfo_t, 128> processBuf;

    nvmlDevice_t dev{};
    if(auto err = nvmlDeviceGetHandleByIndex(devIdx, &dev))
    {
        fprintf(stderr, "Could not get device %u: %s\n", devIdx, nvmlErrorString(err));
        std::exit(1);
    }

    nvmlMemory_t mem{};
    if(auto err = nvmlDeviceGetMemoryInfo(dev, &mem))
    {
        fprintf(stderr, "Could not get memory info: %s\n", nvmlErrorString(err));
        std::exit(1);
    }
    card.memoryTotal = mem.total;
    card.memoryUsage = mem.used;

    nvmlUtilization_t util{};
    if(auto err = nvmlDeviceGetUtilizationRates(dev, &util))
    {
        fprintf(stderr, "Could not get utilization info: %s\n", nvmlErrorString(err));
        std::exit(1);
    }
    card.computeUsagePercent = util.gpu;

    if(auto err = nvmlDeviceGetMinorNumber(dev, &card.minorID))
    {
        fprintf(stderr, "Could not query device ID: %s\n", nvmlErrorString(err));
        std::exit(1);
    }

    snprintf(buf, sizeof(buf), "/dev/nvidia%u", card.minorID);
    struct stat st{};
    if(stat(buf, &st) != 0)
    {
        fprintf(stderr, "Could not query owner of %s: %s\n", buf, strerror(errno));
        std::exit(1);
    }
    card.reservedByUID = st.st_uid;

    unsigned int procCount = processBuf.size();
    if(auto err = nvmlDeviceGetComputeRunningProcesses(dev, &procCount, processBuf.data()))
    {
        fprintf(stderr, "Could not get running processes: %s\n", nvmlErrorString(err));
        procCount = 0;
    }

    card.processes.clear();
    for(unsigned int i = 0; i < procCount; ++i)
    {
        auto& proc = card.processes.emplace_back();
        proc.pid = processBuf[i].pid;
        proc.memory = processBuf[i].usedGpuMemory;

        snprintf(buf, sizeof(buf), "/proc/%u", proc.pid);
        struct stat st{};
        if(stat(buf, &st) != 0)
        {
            card.processes.pop_back();
            continue;
        }

        proc.uid = st.st_uid;
    }

    procCount = processBuf.size();
    if(auto err = nvmlDeviceGetGraphicsRunningProcesses(dev, &procCount, processBuf.data()))
    {
        fprintf(stderr, "Could not get running processes: %s\n", nvmlErrorString(err));
        procCount = 0;
    }

    for(unsigned int i = 0; i < procCount; ++i)
    {
        auto it = std::find_if(card.processes.begin(), card.processes.end(), [&](auto& proc){
            return proc.pid == processBuf[i].pid;
        });

        if(it == card.processes.end())
            it = card.processes.insert(it, {});

        auto& proc = card.processes.emplace_back();
        proc.pid = processBuf[i].pid;
        proc.memory += processBuf[i].usedGpuMemory;

        snprintf(buf, sizeof(buf), "/proc/%u", proc.pid);
        struct stat st{};
        if(stat(buf, &st) != 0)
        {
            card.processes.pop_back();
            continue;
        }

        proc.uid = st.st_uid;
    }


    if(card.reservedByUID != 0)
    {
        for(auto& proc : card.processes)
        {
            if(card.reservedByUID == proc.uid)
                card.lastUsageTime = now;
        }
    }
}

int main(int argc, char** argv)
{
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(sock < 0)
    {
        perror("Could not open unix socket");
        return 1;
    }

    {
        unlink("/var/run/gpu_server.sock");

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, "/var/run/gpu_server.sock");

        if(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            perror("Could not create unix socket at /var/run/gpu_server.sock");
            return 1;
        }

        if(listen(sock, 20) != 0)
        {
            perror("Could not listen()");
            return 1;
        }

        if(chmod("/var/run/gpu_server.sock", 0777) != 0)
        {
            perror("Could not set socket permissions on /var/run/gpu_server.sock");
            return 1;
        }
    }

    if(auto err = nvmlInitWithFlags(0))
    {
        fprintf(stderr, "Could not initialize NVML: %s\n", nvmlErrorString(err));
        return 1;
    }

    unsigned int devices = 0;
    if(auto err = nvmlDeviceGetCount_v2(&devices))
    {
        fprintf(stderr, "Could not list nvidia devices: %s\n", nvmlErrorString(err));
        return 1;
    }

    char buf[1024];

    for(unsigned int devIdx = 0; devIdx < devices; ++devIdx)
    {
        nvmlDevice_t dev{};
        if(auto err = nvmlDeviceGetHandleByIndex(devIdx, &dev))
        {
            fprintf(stderr, "Could not get device %u: %s\n", devIdx, nvmlErrorString(err));
            return 1;
        }

        auto& card = g_cards.emplace_back();

        if(auto err = nvmlDeviceGetName(dev, buf, sizeof(buf)))
        {
            fprintf(stderr, "Could not get device name: %s\n", nvmlErrorString(err));
            return 1;
        }
        card.name = buf;

        if(auto err = nvmlDeviceGetUUID(dev, buf, sizeof(buf)))
        {
            fprintf(stderr, "Could not get card UUID: %s\n", nvmlErrorString(err));
            return 1;
        }
        card.uuid = buf;

        nvmlMemory_t mem{};
        if(auto err = nvmlDeviceGetMemoryInfo(dev, &mem))
        {
            fprintf(stderr, "Could not get memory info: %s\n", nvmlErrorString(err));
            return 1;
        }
        card.memoryTotal = mem.total;
    }

    printf("Initialized with %lu cards.\n", g_cards.size());

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if(timerfd < 0)
    {
        perror("Could not create timer fd");
        return 1;
    }

    {
        itimerspec spec{};
        spec.it_interval.tv_sec = 1;
//         spec.it_interval.tv_nsec = 1000ULL * 1000ULL * 500ULL; // 500ms
        spec.it_value.tv_sec = 0;
        spec.it_value.tv_nsec = 1; // Trigger immediately

        if(timerfd_settime(timerfd, 0, &spec, nullptr) != 0)
        {
            perror("Could not arm timer");
            return 1;
        }
    }

    int epollfd = epoll_create1(EPOLL_CLOEXEC);
    if(epollfd < 0)
    {
        perror("Could not create epoll fd");
        return 1;
    }

    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &sock;
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD, sock, &ev) != 0)
        {
            perror("Could not add socket to epoll");
            return 1;
        }
    }
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &timerfd;
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD, timerfd, &ev) != 0)
        {
            perror("Could not add timer to epoll");
            return 1;
        }
    }

    std::array<epoll_event, 20> events;
    while(1)
    {
        int nfds = epoll_wait(epollfd, events.data(), events.size(), -1);
        if(nfds <= 0)
        {
            perror("epoll_wait() failed");
            return 1;
        }

        std::vector<Client*> deleteList;

        for(int i = 0; i < nfds; ++i)
        {
            auto& ev = events[i];

            if(ev.data.ptr == &sock)
            {
                // Main socket
                int fd = accept(sock, nullptr, nullptr);
                if(fd < 0)
                {
                    perror("Could not accept client");
                    sleep(1);
                    continue;
                }

                if(g_clients.size() > 100)
                {
                    close(fd);
                    continue;
                }

                auto client = std::make_unique<Client>(fd);

                epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.ptr = client.get();

                if(epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) != 0)
                {
                    perror("Could not add client to epoll");
                    continue;
                }

                g_clients.push_back(std::move(client));
            }
            else if(ev.data.ptr == &timerfd)
            {
                std::uint64_t expirations = 0;
                int ret = read(timerfd, &expirations, sizeof(expirations));
                if(ret <= 0)
                {
                    perror("Could not read from timerfd");
                    return 1;
                }

                auto now = std::chrono::steady_clock::now();

                for(auto& client : g_clients)
                {
                    using namespace std::chrono_literals;
                    if(now - client->connectTime > 2s)
                        deleteList.push_back(client.get());
                }

                for(unsigned int devIdx = 0; devIdx < devices; ++devIdx)
                {
                    auto& card = g_cards[devIdx];

                    updateCardFromNVML(devIdx, card, now);

                    using namespace std::chrono_literals;
                    if(card.reservedByUID && now - card.lastUsageTime > 5min)
                    {
                        printf("Returning card %u, no usage for long time\n", devIdx);
                        release(card);
                    }
                }
            }
            else
            {
                // Handle client request
                Client* client = reinterpret_cast<Client*>(ev.data.ptr);

                if(!client->communicate())
                    deleteList.push_back(client);
            }
        }

        // Process client deletion list
        for(auto& toDelete : deleteList)
        {
            auto it = std::find_if(g_clients.begin(), g_clients.end(), [&](auto& client){
                return client.get() == toDelete;
            });

            if(it == g_clients.end())
                continue;

            if(epoll_ctl(epollfd, EPOLL_CTL_DEL, it->get()->fd, nullptr) != 0)
            {
                perror("Could not remove client from epoll list");
            }

            g_clients.erase(it);
        }
    }

    nvmlShutdown();

    return 0;
}

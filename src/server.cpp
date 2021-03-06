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
#include <sstream>

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
#include "priority_queue.h"

namespace
{
    template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
}

struct Client
{
    int fd = -1;
    std::chrono::steady_clock::time_point connectTime;
    int uid = -1;
    int pid = -1;
    bool waitingOnQueue = false;

    explicit Client(int fd);

    ~Client()
    {
        printf("Closing connection to client %d (UID %d)\n", pid, uid);
        if(fd >= 0)
            close(fd);
    };

    // Return false if the client should be deleted
    [[nodiscard]] bool communicate();

    void send(auto&& msg)
    {
        auto [data, out] = zpp::bits::data_out();
        out(msg).or_throw();

        if(::send(fd, data.data(), data.size(), MSG_EOR) != data.size())
            perror("Could not send response");
    }
};

std::vector<Card> g_cards;
std::vector<std::unique_ptr<Client>> g_clients;
PriorityQueue g_jobQueue;
std::size_t gpuLimitPerUser = 8;
std::vector<Client*> deleteList;

void claim(Card& card, int uid, int gid=65534)
{
    if(uid < 0)
        throw std::logic_error{"claim(): Invalid UID"};

    char buf[256];
    snprintf(buf, sizeof(buf), "/dev/nvidia%u", card.minorID);

    if(chown(buf, uid, gid) != 0)
    {
        fprintf(stderr, "Could not set owner of %s to UID %d: %s\n",
            buf, uid, strerror(errno)
        );
        std::exit(1);
    }
    card.reservedByUID = uid;
    card.lastUsageTime = std::chrono::steady_clock::now();

    if(uid == 0)
        printf("Card %d released.\n", card.index);
    else
        printf("Card %d claimed by UID %d.\n", card.index, uid);
}

void release(Card& card)
{
    claim(card, 0, 0);
}

void updateCardFromNVML(unsigned int devIdx, Card& card, const std::chrono::steady_clock::time_point& now = std::chrono::steady_clock::now())
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

void periodicUpdate()
{
    auto now = std::chrono::steady_clock::now();
    for(unsigned int devIdx = 0; devIdx < g_cards.size(); ++devIdx)
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

    for(auto& client : g_clients)
    {
        using namespace std::chrono_literals;
        if(!client->waitingOnQueue && now - client->connectTime > 2s)
            deleteList.push_back(client.get());
    }

    // Check if next jobs are feasible
    g_jobQueue.update();
    while(!g_jobQueue.empty())
    {
        const auto& job = g_jobQueue.front();
        auto it = std::ranges::find_if(g_clients, [&](auto& client){
            return client->pid == job.pid;
        });
        if(it == g_clients.end())
            throw std::logic_error{"Job without client"};
        auto& client = *it;

        // Can we immediately satisfy this request?
        std::vector<int> freeCards;
        for(std::size_t i = 0; i < g_cards.size(); ++i)
        {
            auto& card = g_cards[i];
            if(card.reservedByUID == 0)
                freeCards.push_back(i);
        }

        // Never allow someone to claim all cards
        int alreadyClaimed = std::ranges::count_if(g_cards, [=](auto& card) { return card.reservedByUID == job.uid; });
        if(alreadyClaimed + job.numGPUs > gpuLimitPerUser)
        {
            printf("Sending per-user limit reached\n");
            ClaimResponse resp;
            resp.error = "GPU per-user limit is reached";
            client->send(resp);
            deleteList.push_back(client.get());
        }

        // Not feasible currently
        if(job.numGPUs > freeCards.size())
            break;

        // Feasible!
        printf("Starting job of client %ld\n", job.pid);
        ClaimResponse resp;
        for(unsigned int i = 0; i < job.numGPUs; ++i)
        {
            claim(g_cards[freeCards[i]], job.uid);
            resp.claimedCards.push_back(g_cards[freeCards[i]]);
        }
        client->send(resp);
        deleteList.push_back(client.get());

        g_jobQueue.pop_front();
    }
}

Client::Client(int fd)
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
    pid = cred.pid;
}

// Return false if the client should be deleted
[[nodiscard]] bool Client::communicate()
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

    zpp::bits::in in{buf};

    Request req;
    auto res = in(req);
    if(zpp::bits::failure(res))
    {
        fprintf(stderr, "Client sent request that could not be parsed\n");
        return false;
    }

    return std::visit(overloaded {
        [&](const StatusRequest&) {
            StatusResponse resp;
            resp.cards = g_cards;

            send(resp);
            return false;
        },
        [&](const ClaimRequest& req) {
            if(req.numGPUs > gpuLimitPerUser)
            {
                ClaimResponse resp;
                resp.error = "Your requested GPU count is over the per-user limit.";
                send(resp);
                return false;
            }

            Job job;
            job.numGPUs = req.numGPUs;
            job.pid = pid;
            job.uid = uid;
            g_jobQueue.enqueue(std::move(job));

            periodicUpdate();

            waitingOnQueue = true;
            return true; // keep alive
        },
        [&](const ReleaseRequest& req) {
            std::stringstream errors;
            for(auto& cardIdx : req.gpus)
            {
                if(cardIdx >= g_cards.size())
                {
                    errors << "Invalid card index " << cardIdx << "\n";
                    continue;
                }

                auto& card = g_cards[cardIdx];

                updateCardFromNVML(cardIdx, card);

                if(card.reservedByUID != uid)
                {
                    errors << "Card " << cardIdx << " is not reserved by user\n";
                    continue;
                }

                auto it = std::ranges::find_if(card.processes, [&](const auto& proc){
                    return proc.uid == uid;
                });

                if(it != card.processes.end())
                {
                    errors << "Card " << cardIdx << " is still in use. Maybe you want to kill the process with PID " << it->pid << "?\n";
                    continue;
                }

                release(card);
            }

            send(ReleaseResponse{errors.str()});

            return false;
        },
        [&](auto) {
            fprintf(stderr, "Unhandled command type\n");
            return false;
        },
    }, req);
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

        card.index = g_cards.size() - 1;

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

        card.lastUsageTime = std::chrono::steady_clock::now();
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

                periodicUpdate();
            }
            else
            {
                // Handle client request
                Client* client = reinterpret_cast<Client*>(ev.data.ptr);

                if(!client->communicate())
                {
                    printf("Client::communicate() returned false\n");
                    deleteList.push_back(client);
                }
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

            g_jobQueue.remove(it->get()->pid);

            g_clients.erase(it);
        }
        deleteList.clear();
    }

    nvmlShutdown();

    return 0;
}

// Server
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

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
#include <filesystem>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>

#include <zpp_bits.h>

#include "../protocol.h"
#include "gpu_info.h"
#include "client.h"
#include "overloaded.h"


ServerStatus g_status;

std::vector<std::unique_ptr<Client>> g_clients;

void killRemainingProcesses(const char* cardPath)
{
    // Kill any remaining processes which where not visible with NVML (this can happen sometimes)
    char fusercmd[256];
    snprintf(fusercmd, sizeof(fusercmd), "fuser %s", cardPath);

    FILE* f = popen(fusercmd, "r");
    if(!f)
    {
        perror("Could not call fuser");
        return;
    }

    char buf[256];

    int mypid = getpid();

    while(!feof(f))
    {
        int pid;
        if(fscanf(f, "%d", &pid) != 1)
        {
            fprintf(stderr, "Could not read fuser result\n");
            break;
        }

        if(pid == mypid)
            continue;

        printf("Killing leftover process %d.\n", pid);
        kill(pid, 9);
    }

    fclose(f);
}

void claim(Card& card, int uid, int gid=65534, int pid=-1)
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
    card.clientPIDs = {pid};
    card.lastUsageTime = std::chrono::steady_clock::now();

    if(uid == 0)
    {
        killRemainingProcesses(buf);
        printf("Card %d released.\n", card.index);
        card.lockedUntilUpdate = true;
    }
    else
    {
        struct passwd *pws;
        pws = getpwuid(uid);
        printf("Card %d claimed by UID %d (%s).\n", card.index, uid, pws ? pws->pw_name : "unknown");
    }
}

void release(Card& card)
{
    claim(card, 0, 0);
}

void releaseFromClient(Card& card, int clientPID)
{
    std::erase(card.clientPIDs, clientPID);
    if(card.processes.empty() && card.clientPIDs.empty())
        release(card);
}


void periodicUpdate()
{
    auto now = std::chrono::steady_clock::now();
    for(unsigned int devIdx = 0; devIdx < g_status.cards.size(); ++devIdx)
    {
        auto& card = g_status.cards[devIdx];

        gpu_info::update(card, now);

        if(card.processes.empty() && !card.clientPIDs.empty())
        {
            std::vector<int> erasePIDs;
            for(auto clientPID : card.clientPIDs)
            {
                // Is our client still alive?
                if(kill(clientPID, 0) != 0)
                {
                    printf("Returning card %u, client with PID %d is not alive anymore\n", devIdx, clientPID);
                    erasePIDs.push_back(clientPID);
                }
            }

            for(auto pid : erasePIDs)
                releaseFromClient(card, pid);
        }

        using namespace std::chrono_literals;
        if(card.reservedByUID && now - card.lastUsageTime > 1min)
        {
            printf("Returning card %u, no usage for long time\n", devIdx);
            release(card);
        }
    }

    g_status.maintenance = std::filesystem::exists("/var/run/gpu_claim_maintenance");

    // Check if next jobs are feasible
    while(!g_status.queue.empty())
    {
        const auto& job = g_status.queue.front();
        auto it = std::ranges::find_if(g_clients, [&](auto& client){
            return client->pid == job.pid;
        });
        if(it == g_clients.end())
            throw std::logic_error{"Job without client"};
        auto& client = *it;

        if(g_status.maintenance)
        {
            printf("Sending maintenance notice\n");
            ClaimResponse resp;
            resp.error = "Server is undergoing maintenance and will not accept new jobs.";
            client->send(resp);
            g_status.queue.pop_front();
            continue;
        }

        // Can we immediately satisfy this request?
        std::vector<int> freeCards;
        for(std::size_t i = 0; i < g_status.cards.size(); ++i)
        {
            auto& card = g_status.cards[i];
            if(card.reservedByUID == 0 && !card.lockedUntilUpdate && card.processes.empty())
                freeCards.push_back(i);
        }

        // Never allow someone to claim all cards
        int alreadyClaimed = std::ranges::count_if(g_status.cards, [=](auto& card) { return card.reservedByUID == job.uid; });
        if(alreadyClaimed + job.numGPUs > gpuLimitPerUser)
        {
            printf("Sending per-user limit reached\n");
            ClaimResponse resp;
            resp.error = "GPU per-user limit is reached";
            client->send(resp);
            g_status.queue.pop_front();
            continue;
        }

        // Not feasible currently
        if(job.numGPUs > freeCards.size())
            break;

        // Feasible!
        printf("Starting job of client %ld\n", job.pid);
        ClaimResponse resp;
        for(unsigned int i = 0; i < job.numGPUs; ++i)
        {
            claim(g_status.cards[freeCards[i]], job.uid, 65534, job.pid);
            resp.claimedCards.push_back(g_status.cards[freeCards[i]]);
        }
        client->send(resp);

        g_status.queue.pop_front();
    }
}



int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

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

    if(auto cards = gpu_info::init())
        g_status.cards = *cards;
    else
        return 1;

    printf("Initialized with %lu cards.\n", g_status.cards.size());

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if(timerfd < 0)
    {
        perror("Could not create timer fd");
        return 1;
    }

    {
        itimerspec spec{};
        spec.it_interval.tv_sec = 1;
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

    std::vector<Client*> deleteList;

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

                auto action = client->communicate(g_status);
                std::visit(overloaded{
                    [&](const Client::Keep&) {},
                    [&](const Client::Delete&) { deleteList.push_back(client); },
                    [&](const Client::EnqueueJob& ac) {
                        g_status.queue.push_back(ac.job);
                        periodicUpdate();
                    },
                    [&](const Client::CoRunCards& ac) {
                        for(auto& cardIdx : ac.cards)
                        {
                            g_status.cards[cardIdx].clientPIDs.push_back(client->pid);
                        }
                    },
                    [&](const Client::ReleaseCards& ac) {
                        periodicUpdate();
                        for(auto& cardIdx : ac.cards)
                        {
                            releaseFromClient(g_status.cards[cardIdx], client->pid);
                        }
                    }
                }, action);
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

            auto client = it->get();

            if(epoll_ctl(epollfd, EPOLL_CTL_DEL, client->fd, nullptr) != 0)
            {
                perror("Could not remove client from epoll list");
            }

            g_status.queue.remove_if([&](auto& job){
                return job.pid == client->pid;
            });

            for(auto& card : g_status.cards)
            {
                if(std::ranges::find(card.clientPIDs, client->pid) != card.clientPIDs.end())
                {
                    printf("Releasing card %d, client with PID %d disconnected\n", card.index, client->pid);
                    releaseFromClient(card, client->pid);
                }
            }

            g_clients.erase(it);
        }

        deleteList.clear();
    }

    gpu_info::shutdown();

    return 0;
}

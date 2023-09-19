// Client connection
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "client.h"

#include <sstream>

#include "overloaded.h"

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

Client::Action Client::communicate(const ServerStatus& serverStatus)
{
    // If authentication failed (see above), don't accept any commands.
    if(uid < 0)
    {
        printf("Authentication failed.\n");
        return Delete{};
    }

    std::array<uint8_t, 512> buf;

    int ret = read(fd, buf.data(), buf.size());
    if(ret == 0)
    {
        // Client has closed connection
        return Delete{};
    }
    if(ret < 0)
    {
        perror("Could not read from client");
        return Delete{};
    }

    zpp::bits::in in{buf};

    Request req;
    auto res = in(req);
    if(zpp::bits::failure(res))
    {
        fprintf(stderr, "Client sent request that could not be parsed\n");
        return Delete{};
    }

    return std::visit(overloaded {
        [&](const StatusRequest&) -> Action {
            StatusResponse resp;
            resp.cards = serverStatus.cards;
            resp.queue.reserve(serverStatus.queue.size());
            for(auto& job : serverStatus.queue)
                resp.queue.push_back(job);
            resp.maintenance = serverStatus.maintenance;

            send(resp);
            return Keep{};
        },
        [&](const ClaimRequest& req) -> Action {
            if(req.numGPUs > gpuLimitPerUser)
            {
                ClaimResponse resp;
                resp.error = "Your requested GPU count is over the per-user limit.";
                send(resp);
                return Delete{};
            }

            Job job;
            job.numGPUs = req.numGPUs;
            job.pid = pid;
            job.uid = uid;
            job.submissionTime = std::chrono::system_clock::now();
            return EnqueueJob{std::move(job)};
        },
        [&](const CoRunRequest& req) -> Action {
            for(auto& cardIdx : req.gpus)
            {
                if(cardIdx >= serverStatus.cards.size())
                {
                    ClaimResponse resp;
                    resp.error = "Invalid GPU number";
                    send(resp);
                    return Delete{};
                }

                auto& card = serverStatus.cards[cardIdx];
                if(card.reservedByUID != uid)
                {
                    ClaimResponse resp;
                    std::stringstream ss;
                    ss << "Card " << cardIdx << " is not reserved by you";
                    resp.error = ss.str();
                    send(resp);
                    return Delete{};
                }
            }

            ClaimResponse resp;
            for(auto c : req.gpus)
                resp.claimedCards.push_back(serverStatus.cards[c]);
            send(resp);

            return CoRunCards{req.gpus};
        },
        [&](const ReleaseRequest& req) -> Action {
            std::stringstream errors;
            ReleaseCards action;

            for(auto& cardIdx : req.gpus)
            {
                if(cardIdx >= serverStatus.cards.size())
                {
                    errors << "Invalid card index " << cardIdx << "\n";
                    continue;
                }

                auto& card = serverStatus.cards[cardIdx];

                if(card.reservedByUID != uid)
                {
                    errors << "Card " << cardIdx << " is not reserved by user\n";
                    continue;
                }

                auto cit = std::ranges::find(card.clientPIDs, pid);
                if(cit == card.clientPIDs.end())
                {
                    errors << "Card " << cardIdx << " is not reserved by your PID\n";
                    continue;
                }

                if(card.clientPIDs.size() == 1)
                {
                    auto it = std::ranges::find_if(card.processes, [&](const auto& proc){
                        return proc.uid == uid;
                    });

                    if(it != card.processes.end())
                    {
                        errors << "Card " << cardIdx << " is still in use. Maybe you want to kill the process with PID " << it->pid << "?\n";
                        continue;
                    }
                }

                action.cards.push_back(card.index);
            }

            send(ReleaseResponse{errors.str()});

            if(errors.str().empty())
                return action;
            else
                return Keep{};
        },
        [&](auto) -> Action {
            fprintf(stderr, "Unhandled command type\n");
            return Delete{};
        },
    }, req);
}

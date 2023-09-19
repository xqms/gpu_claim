// Client connection
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef CLIENT_H
#define CLIENT_H

#include <chrono>
#include <cstdio>

#include <sys/types.h>
#include <sys/socket.h>

#include <unistd.h>

#include <zpp_bits.h>

#include "../protocol.h"

struct Client
{
    int fd = -1;
    std::chrono::steady_clock::time_point connectTime;
    int uid = -1;
    int pid = -1;

    explicit Client(int fd);

    ~Client()
    {
        if(fd >= 0)
            close(fd);
    };

    struct Keep {};
    struct Delete {};
    struct EnqueueJob
    {
        Job job;
    };
    struct CoRunCards
    {
        std::vector<std::uint32_t> cards;
    };
    struct ReleaseCards
    {
        std::vector<int> cards;
    };

    using Action = std::variant<Keep, Delete, EnqueueJob, CoRunCards, ReleaseCards>;

    // Return false if the client should be deleted
    [[nodiscard]] Action communicate(const ServerStatus& serverStatus);

    void send(auto&& msg)
    {
        auto [data, out] = zpp::bits::data_out();
        out(msg).or_throw();

        if(::send(fd, data.data(), data.size(), MSG_EOR) != data.size())
            perror("Could not send response");
    }
};

#endif

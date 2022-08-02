// Client/server protocol
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <vector>
#include <cstdint>
#include <string>
#include <chrono>

#include <zpp_bits.h>

struct Process
{
    int uid = 0;
    int pid = 0;
    std::uint64_t memory = 0;
};

struct Card
{
    unsigned int index = 0;
    unsigned int minorID = 0;
    std::string name;
    std::string uuid;
    std::uint8_t computeUsagePercent = 0;
    std::uint64_t memoryTotal = 0.0f;
    std::uint64_t memoryUsage = 0.0f;

    int reservedByUID = 0;
    std::vector<Process> processes;

    std::chrono::steady_clock::time_point lastUsageTime;
};

struct Job
{
    std::int64_t uid = 0;
    std::int64_t pid = 0;
    std::int64_t numGPUs = 0;
    float priority;
    std::chrono::system_clock::time_point submissionTime;
};

struct StatusRequest
{
};
struct StatusResponse
{
    std::vector<Card> cards;
    std::vector<Job> jobsInQueue;
};

struct ClaimRequest
{
    std::uint32_t numGPUs = 0;
    bool wait = false;
};
struct ClaimResponse
{
    std::vector<Card> claimedCards;
    std::string error;
};

struct ReleaseRequest
{
    std::vector<std::uint32_t> gpus;
};
struct ReleaseResponse
{
    std::string errors;
};

using Request = std::variant<StatusRequest, ClaimRequest, ReleaseRequest>;

namespace std
{
namespace chrono
{

template<class Clock, class Dur>
constexpr auto serialize(auto& archive, std::chrono::time_point<Clock, Dur>& tp)
{
    using archive_type = std::remove_cvref_t<decltype(archive)>;

    if constexpr (archive_type::kind() == zpp::bits::kind::in)
    {
        std::uint64_t val = 0;
        auto res = archive(val);
        if(zpp::bits::failure(res))
            return res;

        tp = std::chrono::time_point<Clock, Dur>{std::chrono::milliseconds{val}};

        return res;
    }
    else
    {
        std::uint64_t val = std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
        return archive(val);
    }
}

template<class Clock, class Dur>
constexpr auto serialize(auto& archive, const std::chrono::time_point<Clock, Dur>& tp)
{
    using archive_type = std::remove_cvref_t<decltype(archive)>;
    static_assert(archive_type::kind() == zpp::bits::kind::out);

    std::uint64_t val = std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
    return archive(val);
}
}
}

#endif

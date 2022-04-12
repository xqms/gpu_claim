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

struct StatusRequest
{
};
struct StatusResponse
{
    std::vector<Card> cards;
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



struct Request
{
    // You can only append to this list. Never remove, otherwise you break
    // protocol compatibility.
    std::variant<StatusRequest, ClaimRequest, ReleaseRequest> data;
};

namespace std
{
namespace chrono
{
constexpr auto serialize(auto& archive, std::chrono::steady_clock::time_point& tp)
{
    using archive_type = std::remove_cvref_t<decltype(archive)>;

    if constexpr (archive_type::kind() == zpp::bits::kind::in)
    {
        std::uint64_t val = 0;
        auto res = archive(val);
        if(zpp::bits::failure(res))
            return res;

        tp = std::chrono::steady_clock::time_point{std::chrono::milliseconds{val}};

        return res;
    }
    else
    {
        std::uint64_t val = std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
        return archive(val);
    }
}

constexpr auto serialize(auto& archive, const std::chrono::steady_clock::time_point& tp)
{
    using archive_type = std::remove_cvref_t<decltype(archive)>;
    static_assert(archive_type::kind() == zpp::bits::kind::out);

    std::uint64_t val = std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
    return archive(val);
}
}
}

#endif

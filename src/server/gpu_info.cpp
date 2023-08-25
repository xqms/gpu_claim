// Collect information from GPUs using NVIDIA NVML
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "gpu_info.h"

#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include <nvml.h>

#include <sys/stat.h>

namespace gpu_info
{

std::optional<std::vector<Card>> init()
{
    std::vector<Card> cards;

    // Because nvidia is stupid, NVML will reset the owner & permissions on init.
    // Let's try to restore permissions afterwards.
    std::vector<unsigned int> owner;
    for(unsigned int i = 0;; ++i)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "/dev/nvidia%u", i);
        struct stat st{};
        if(stat(buf, &st) != 0)
            break;

        owner.push_back(st.st_uid);
    }

    if(auto err = nvmlInitWithFlags(0))
    {
        fprintf(stderr, "Could not initialize NVML: %s\n", nvmlErrorString(err));
        return {};
    }

    unsigned int devices = 0;
    if(auto err = nvmlDeviceGetCount_v2(&devices))
    {
        fprintf(stderr, "Could not list nvidia devices: %s\n", nvmlErrorString(err));
        return {};
    }

    char buf[1024];

    for(unsigned int devIdx = 0; devIdx < devices; ++devIdx)
    {
        nvmlDevice_t dev{};
        if(auto err = nvmlDeviceGetHandleByIndex(devIdx, &dev))
        {
            fprintf(stderr, "Could not get device %u: %s\n", devIdx, nvmlErrorString(err));
            return {};
        }

        auto& card = cards.emplace_back();

        card.index = cards.size() - 1;

        if(auto err = nvmlDeviceGetName(dev, buf, sizeof(buf)))
        {
            fprintf(stderr, "Could not get device name: %s\n", nvmlErrorString(err));
            return {};
        }
        card.name = buf;

        if(auto err = nvmlDeviceGetUUID(dev, buf, sizeof(buf)))
        {
            fprintf(stderr, "Could not get card UUID: %s\n", nvmlErrorString(err));
            return {};
        }
        card.uuid = buf;

        nvmlMemory_v2_t mem{};
        mem.version = nvmlMemory_v2;

        if(auto err = nvmlDeviceGetMemoryInfo_v2(dev, &mem))
        {
            fprintf(stderr, "Could not get memory info: %s\n", nvmlErrorString(err));
            return {};
        }
        card.memoryTotal = mem.total;

        if(auto err = nvmlDeviceGetMinorNumber(dev, &card.minorID))
        {
            fprintf(stderr, "Could not query device ID: %s\n", nvmlErrorString(err));
            std::exit(1);
        }

        // Restore owner
        if(card.minorID < owner.size())
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "/dev/nvidia%u", card.minorID);
            chown(buf, owner[card.minorID], 65534);
        }

        card.lastUsageTime = std::chrono::steady_clock::now();
    }

    return cards;
}

void update(Card& card, const std::chrono::steady_clock::time_point& now)
{
    char buf[1024];
    std::array<nvmlProcessInfo_v2_t, 128> processBuf;

    nvmlDevice_t dev{};
    if(auto err = nvmlDeviceGetHandleByIndex_v2(card.index, &dev))
    {
        fprintf(stderr, "Could not get device %u: %s\n", card.index, nvmlErrorString(err));
        std::exit(1);
    }

    nvmlMemory_v2_t mem{};
    mem.version = nvmlMemory_v2;
    if(auto err = nvmlDeviceGetMemoryInfo_v2(dev, &mem))
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

    unsigned int temp = 0;
    if(auto err = nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp))
    {
        fprintf(stderr, "Could not get temperature: %s\n", nvmlErrorString(err));
        std::exit(1);
    }
    card.temperatureCelsius = temp;

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

    // Reset mode to rw- --- ---
    if(st.st_mode & (S_IWGRP | S_IRGRP | S_IWOTH | S_IROTH))
    {
        if(chmod(buf, 0600) != 0)
        {
            fprintf(stderr, "Could not set mode of %s: %s\n", buf, strerror(errno));
        }
    }

    unsigned int procCount = processBuf.size();
    if(auto err = nvmlDeviceGetComputeRunningProcesses_v2(dev, &procCount, processBuf.data()))
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
            fprintf(stderr, "Could not stat %s: %s\n", buf, strerror(errno));
            card.processes.pop_back();
            continue;
        }

        proc.uid = st.st_uid;
    }

    procCount = processBuf.size();
    if(auto err = nvmlDeviceGetGraphicsRunningProcesses_v2(dev, &procCount, processBuf.data()))
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
            fprintf(stderr, "Could not stat %s: %s\n", buf, strerror(errno));
            card.processes.pop_back();
            continue;
        }

        proc.uid = st.st_uid;
    }

    if(!card.processes.empty())
        card.lastUsageTime = now;

    card.lockedUntilUpdate = false;
}

void shutdown()
{
    nvmlShutdown();
}

}

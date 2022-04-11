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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

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
    std::uint8_t computeUsagePercent = 0;
    std::uint64_t memoryTotal = 0.0f;
    std::uint64_t memoryUsage = 0.0f;

    int reservedByUID = 0;
    std::vector<Process> processes;
};

int main(int argc, char** argv)
{
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

    std::vector<Card> cards;

    for(unsigned int devIdx = 0; devIdx < devices; ++devIdx)
    {
        nvmlDevice_t dev{};
        if(auto err = nvmlDeviceGetHandleByIndex(devIdx, &dev))
        {
            fprintf(stderr, "Could not get device %u: %s\n", devIdx, nvmlErrorString(err));
            return 1;
        }

        auto& card = cards.emplace_back();

        if(auto err = nvmlDeviceGetName(dev, buf, sizeof(buf)))
        {
            fprintf(stderr, "Could not get device name: %s\n", nvmlErrorString(err));
            return 1;
        }
        card.name = buf;

        nvmlMemory_t mem{};
        if(auto err = nvmlDeviceGetMemoryInfo(dev, &mem))
        {
            fprintf(stderr, "Could not get memory info: %s\n", nvmlErrorString(err));
            return 1;
        }
        card.memoryTotal = mem.total;

        if(auto err = nvmlDeviceGetMinorNumber(dev, &card.minorID))
        {
            fprintf(stderr, "Could not query device ID: %s\n", nvmlErrorString(err));
            return 1;
        }

        snprintf(buf, sizeof(buf), "/dev/nvidia%u", card.minorID);
        struct stat st{};
        if(stat(buf, &st) != 0)
        {
            fprintf(stderr, "Could not query owner of %s: %s\n", buf, strerror(errno));
            return 1;
        }
        card.reservedByUID = st.st_uid;
    }

    printf("Initialized with %lu cards.\n", cards.size());

    std::array<nvmlProcessInfo_t, 128> processBuf;

    while(1)
    {
        printf("=============================\n");
        for(unsigned int devIdx = 0; devIdx < devices; ++devIdx)
        {
            auto& card = cards[devIdx];

            nvmlDevice_t dev{};
            if(auto err = nvmlDeviceGetHandleByIndex(devIdx, &dev))
            {
                fprintf(stderr, "Could not get device %u: %s\n", devIdx, nvmlErrorString(err));
                return 1;
            }

            nvmlMemory_t mem{};
            if(auto err = nvmlDeviceGetMemoryInfo(dev, &mem))
            {
                fprintf(stderr, "Could not get memory info: %s\n", nvmlErrorString(err));
                return 1;
            }
            card.memoryTotal = mem.total;
            card.memoryUsage = mem.used;

            nvmlUtilization_t util{};
            if(auto err = nvmlDeviceGetUtilizationRates(dev, &util))
            {
                fprintf(stderr, "Could not get utilization info: %s\n", nvmlErrorString(err));
                return 1;
            }
            card.computeUsagePercent = util.gpu;

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

            struct passwd *pws;
            pws = getpwuid(card.reservedByUID);

            printf("[%u] %s | %2d%% | %6lu / %6lu MB | %s |",
                devIdx, card.name.c_str(),
                card.computeUsagePercent,
                card.memoryUsage / 1000000UL, card.memoryTotal / 1000000UL,
                card.reservedByUID ? pws->pw_name : "free"
            );

            for(auto& proc : card.processes)
            {
                struct passwd *pws;
                pws = getpwuid(proc.uid);

                printf(" %s(%luM)",
                    pws->pw_name, proc.memory / 1000000UL
                );
            }
            printf("\n");
        }

        usleep(500 * 1000);
    }

    nvmlShutdown();

    return 0;
}

// gpu client
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "protocol.h"

#include <boost/program_options.hpp>

#include <cstdio>
#include <iostream>
#include <filesystem>
#include <regex>

#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <pwd.h>

using namespace std::chrono_literals;

class Connection
{
public:
    Connection()
    {
        m_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if(m_fd < 0)
        {
            fprintf(stderr, "Could not connect to gpu_server. Please contact the system administrators.\n");
            std::exit(1);
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, "/var/run/gpu_server.sock");
        if(connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            fprintf(stderr, "Could not connect to gpu_server. Please contact the system administrators.\n");
            std::exit(1);
        }
    }

    ~Connection()
    {
        if(m_fd >= 0)
            close(m_fd);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void send(const Request& req)
    {
        auto [data, out] = zpp::bits::data_out();
        out(req).or_throw();

        if(::send(m_fd, data.data(), data.size(), MSG_EOR) != data.size())
        {
            perror("Could not send data to gpu_server");
            fprintf(stderr, "Please contact the system adminstrator.\n");
            std::exit(1);
        }
    }

    void receive(auto& resp)
    {
        std::vector<uint8_t> data(4096);

        iovec iov{};
        iov.iov_base = data.data();
        iov.iov_len = data.size();

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        int ret = recvmsg(m_fd, &msg, 0);
        if(ret <= 0)
        {
            perror("Could not receive data from gpu_server");
            fprintf(stderr, "Please contact the system adminstrator.\n");
            std::exit(1);
        }

        if(msg.msg_flags & MSG_TRUNC)
        {
            fprintf(stderr, "Message was truncated on receive.\n");
            fprintf(stderr, "Please contact the system adminstrator.\n");
            std::exit(1);
        }

        data.resize(ret);
        zpp::bits::in in{data};
        in(resp).or_throw();
    }

    bool waitForReply(const std::chrono::steady_clock::duration& timeout)
    {
        using namespace std::chrono;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);

        std::uint64_t us = duration_cast<microseconds>(timeout).count();

        timeval tv{};
        tv.tv_sec = us / 1000000UL;
        tv.tv_usec = us % 1000000UL;

        int ret = select(m_fd+1, &fds, nullptr, nullptr, &tv);
        if(ret < 0)
        {
            perror("Could not select()");
            std::exit(1);
        }

        return ret != 0;
    }

private:
    int m_fd = -1;
};

bool g_caughtSigint = false;
void handle_sigint(int)
{
    g_caughtSigint = true;
}

std::filesystem::path path_to_install_prefix()
{
    char self[PATH_MAX] = {};
    int nchar = readlink("/proc/self/exe", self, sizeof(self)-1);
    if(nchar < 0 || nchar == sizeof(self)-1)
    {
        perror("Could not readlink /proc/self/exe");
        std::exit(1);
    }

    self[nchar] = 0;

    return std::filesystem::path{self}.parent_path().parent_path();
}

int main(int argc, char** argv)
{
    namespace po = boost::program_options;

    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Help")
        ("version,v", "Display version")
        ("num-cards,n", po::value<unsigned int>()->default_value(1)->value_name("N"), "Number of GPUs to claim")
        ("no-isolation", "Disable device isolation")
    ;

    po::options_description hidden{"Hidden"};
    hidden.add_options()
        ("command", po::value<std::string>()->default_value("status"), "Command")
    ;

    po::options_description allOptions;
    allOptions.add(desc).add(hidden);

    po::positional_options_description p;
    p.add("command", 1);

    po::variables_map vm;

    // Hack: the first option after "run" is the
    // start of the command, which we don't want to parse.
    int startOfRunArgs = argc;
    {
        for(int i = 1; i < argc; ++i)
        {
            if(strcmp(argv[i], "run") == 0)
            {
                startOfRunArgs = i + 1;
                break;
            }
        }
    }

    po::store(po::command_line_parser(startOfRunArgs, argv).options(allOptions).positional(p).run(), vm);

    if(vm.count("help"))
    {
        fprintf(stderr, "Usage: gpu <command> [options]\n"
            "Available commands:\n"
            "  gpu status:\n"
            "    List current GPU allocation & status\n"
            "  gpu claim:\n"
            "    Claim one or more GPUs\n"
            "  gpu run [options] <cmd>:\n"
            "    Run cmd one or more GPUs. Use gpu run -nX <cmd> to use multiple GPUs.\n"
            "\n"
            "Available options:\n"
        );
        std::cerr << desc << "\n";
        return 1;
    }

    if(vm.count("version"))
    {
        fprintf(stderr, "gpu version: %d.%d.%d\n",
            VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH
        );
        return 1;
    }

    po::notify(vm);

    auto installPath = path_to_install_prefix();

    std::string command = vm["command"].as<std::string>();
    if(command == "status")
    {
        Connection conn;

        Request req{StatusRequest{}};
        conn.send(req);

        StatusResponse resp;
        conn.receive(resp);

        auto now = std::chrono::steady_clock::now();

        for(std::size_t i = 0; i < resp.cards.size(); ++i)
        {
            auto& card = resp.cards[i];

            printf("[%lu] %s │ %3d%% %3d°C │ %6lu / %6lu MB │",
                i, card.name.c_str(),
                card.computeUsagePercent, card.temperatureCelsius,
                card.memoryUsage / 1000000UL, card.memoryTotal / 1000000UL
            );

            struct passwd *pws;
            pws = getpwuid(card.reservedByUID);
            if(card.reservedByUID == 0)
            {
                if(card.processes.empty())
                    printf("%27s │", "free");
                else
                    printf("%27s │", "waiting for exit");
            }
            else
            {
                auto idleTime = now - card.lastUsageTime;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(idleTime);

                bool used = std::ranges::any_of(card.processes, [&](auto& proc){
                    return proc.uid == card.reservedByUID;
                });

                if(used)
                    printf("%15s   (running) │", pws ? pws->pw_name : "unknown");
                else
                    printf("%15s (idle %ldmin) │", pws ? pws->pw_name : "unknown", minutes.count());
            }

            for(auto& proc : card.processes)
            {
                struct passwd *pws;
                pws = getpwuid(proc.uid);

                printf(" %s(PID %d, %luM)",
                    pws->pw_name, proc.pid, proc.memory / 1000000UL
                );
            }
            printf("\n");
        }

        if(!resp.jobsInQueue.empty())
        {
            printf("\n");
            printf("Waiting jobs:\n");
            for(auto& job : resp.jobsInQueue)
            {
                struct passwd *pws;
                pws = getpwuid(job.uid);

                std::time_t t = std::chrono::system_clock::to_time_t(job.submissionTime);

                std::stringstream ss;
                ss << std::put_time(std::localtime(&t), "%F %R");

                printf(" - %s %15s: %ld GPU(s)\n",
                    ss.str().c_str(),
                    pws->pw_name, job.numGPUs
                );
            }
        }

        if(resp.maintenance)
        {
            printf("\n");
            printf("============================================================================\n");
            printf("The server is undergoing maintenance and currently does not accept new jobs.\n");
            printf("============================================================================\n");
        }
    }
    else if(command == "claim")
    {
        Connection conn;

        Request req{ClaimRequest{vm["num-cards"].as<unsigned int>(), true}};
        conn.send(req);

        ClaimResponse resp;
        conn.receive(resp);

        if(resp.claimedCards.empty())
        {
            fprintf(stderr, "Could not claim GPUs: %s\n", resp.error.c_str());
            return 1;
        }

        printf("Claimed %lu GPUs:\n", resp.claimedCards.size());
        for(auto& card : resp.claimedCards)
            printf(" - %s\n", card.name.c_str());
        printf("\n");

        printf("Use with:\n");
        printf("export CUDA_VISIBLE_DEVICES=");
        for(std::size_t i = 0; i < resp.claimedCards.size(); ++i)
        {
            fputs(resp.claimedCards[i].uuid.c_str(), stdout);
            if(i != resp.claimedCards.size()-1)
                fputc(',', stdout);
        }
        printf("\n");
    }
    else if(command == "run")
    {
        if(startOfRunArgs == argc)
        {
            fprintf(stderr, "Need command to run.\n");
            return 1;
        }

        bool doHideDevices = !vm.count("no-isolation");
        auto hide_devices = installPath / "lib/gpu/hide_devices";
        if(!std::filesystem::exists(hide_devices))
        {
            fprintf(stderr, "Could not find hide_devices helper (expected it at %s)\n", hide_devices.c_str());
            return 1;
        }

        std::uint32_t nGPUs = vm["num-cards"].as<unsigned int>();

        ClaimResponse resp;
        {
            Connection conn;

            Request req{ClaimRequest{nGPUs, true}};
            conn.send(req);

            bool hadToWait = false;
            if(!conn.waitForReply(500ms))
            {
                printf("gpu: Waiting for free cards... Use 'gpu' in another shell to see the job queue.\n");
                hadToWait = true;
            }

            conn.receive(resp);

            if(resp.claimedCards.empty())
            {
                fprintf(stderr, "Could not claim GPUs: %s\n", resp.error.c_str());
                return 1;
            }

            if(hadToWait)
                printf("gpu: Success! Starting user command.\n");
        }

        std::stringstream ss;
        for(std::size_t i = 0; i < resp.claimedCards.size(); ++i)
        {
            ss << resp.claimedCards[i].uuid;
            if(i != resp.claimedCards.size()-1)
                ss << ",";
        }
        std::string devicesString = ss.str();

        int pid = fork();
        if(pid < 0)
        {
            perror("Could not fork");
            std::abort();
        }

        if(pid == 0)
        {
            setenv("CUDA_VISIBLE_DEVICES", devicesString.c_str(), 1);

            // This shows up in the standard Debian/Ubuntu shell prompt
            setenv("debian_chroot", "GPU shell", 1);

            std::vector<char*> args;
            std::string executable;

            // Add all devices that we need to hide to the argument list
            if(doHideDevices)
            {
                // argv[0]
                executable = hide_devices;
                args.push_back(strdup("hide_devices"));

                auto deviceRegex = std::regex{"nvidia(\\d+)"};
                for(auto& entry : std::filesystem::directory_iterator{"/dev"})
                {
                    auto filename = entry.path().filename().string();

                    std::smatch match;
                    if(!std::regex_match(filename, match, deviceRegex))
                        continue;

                    unsigned long x = std::stoul(match[1]);

                    auto it = std::find_if(resp.claimedCards.begin(), resp.claimedCards.end(), [&](auto& card){ return card.minorID == x; });
                    if(it == resp.claimedCards.end())
                        args.push_back(strdup(filename.c_str()));
                }

                // Everything after this gets executed inside the "container"
                args.push_back(strdup("--"));
            }
            else
                executable = argv[startOfRunArgs];

            for(int i = startOfRunArgs; i < argc; ++i)
                args.push_back(strdup(argv[i]));

            args.push_back(nullptr);

            if(execvp(executable.c_str(), args.data()) != 0)
            {
                perror("Could not execute command");
                std::exit(1);
            }

            // Should not reach here
            std::abort();
        }

        signal(SIGINT, &handle_sigint);

        bool killed = false;
        while(true)
        {
            if(g_caughtSigint && !killed)
            {
                fprintf(stderr, "[gpu] Caught SIGINT, propagating to child process...\n");
                kill(pid, SIGINT);
                killed = true;
            }

            int status = 0;
            int ret = waitpid(pid, &status, WNOHANG);
            if(ret < 0)
            {
                perror("Could not wait for child process");
                std::exit(1);
            }
            if(ret > 0)
                break;

            usleep(200 * 1000);
        }

        {
            Connection conn;

            ReleaseRequest params;
            for(auto& card : resp.claimedCards)
                params.gpus.push_back(card.index);

            Request req{params};
            conn.send(req);

            ReleaseResponse resp;
            conn.receive(resp);

            if(!resp.errors.empty())
            {
                fprintf(stderr, "Could not release GPUs:\n%s\n", resp.errors.c_str());
                return 1;
            }
        }
    }
    else
    {
        fprintf(stderr, "Unknown command '%s'. Try --help.\n", command.c_str());
        return 1;
    }

    return 0;
}

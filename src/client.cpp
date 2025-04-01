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
    class Error : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    Connection()
    {
        m_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
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
            std::stringstream ss;
            ss << "Could not send data to gpu_server: " << strerror(errno) << "\n";
            ss << "Please contact the system administrator.\n";
            throw Error{ss.str()};
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
        ("card,c", po::value<std::vector<std::uint32_t>>(), "Specific card(s) to run on. These must already belong to you (you are running another job on them)")
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
            "  gpu [options] run <cmd>:\n"
            "    Run cmd one or more GPUs.\n"
            "    Single GPU:\n"
            "      gpu run <cmd>\n"
            "    Multi-GPU (replace 2 by number of GPUs):\n"
            "      gpu -n 2 run <cmd>\n"
            "    Run on the same GPU as another command you are already running on GPU 3:\n"
            "      gpu --card 3 run <cmd>\n"
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
                    printf("%28s │", "free");
                else
                    printf("%28s │", "waiting for exit");
            }
            else
            {
                auto idleTime = now - card.lastUsageTime;
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(idleTime);

                bool used = std::ranges::any_of(card.processes, [&](auto& proc){
                    return proc.uid == card.reservedByUID;
                });

                if(used)
                    printf("%15s    (running) │", pws ? pws->pw_name : "unknown");
                else
                    printf("%15s (idle %2ldsec) │", pws ? pws->pw_name : "unknown", seconds.count());
            }

            for(auto& proc : card.processes)
            {
                printf(" %d(%luM)",
                    proc.pid, proc.memory / 1000000UL
                );
            }
            printf("\n");
        }

        if(!resp.queue.empty())
        {
            printf("\n");
            printf("Waiting jobs:\n");
            for(auto& job : resp.queue)
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
    else if(command == "run")
    {
        if(startOfRunArgs == argc)
        {
            fprintf(stderr, "Need command to run.\n");
            return 1;
        }

        bool doHideDevices = !vm.count("no-isolation");
        auto gpu_container = installPath / "lib/gpu/gpu_container";
        if(!std::filesystem::exists(gpu_container))
        {
            fprintf(stderr, "Could not find gpu_container helper (expected it at %s)\n", gpu_container.c_str());
            return 1;
        }

        std::uint32_t nGPUs = vm["num-cards"].as<unsigned int>();

        Connection conn;

        ClaimResponse resp;
        if(vm.count("card") == 0)
        {
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
        else
        {
            std::vector<std::uint32_t> specificGPUs = vm["card"].as<std::vector<std::uint32_t>>();
            Request req{CoRunRequest{specificGPUs}};
            conn.send(req);

            conn.receive(resp);

            if(resp.claimedCards.empty())
            {
                fprintf(stderr, "Could not claim GPUs: %s\n", resp.error.c_str());
                return 1;
            }
        }

        std::stringstream ss;
        for(std::size_t i = 0; i < resp.claimedCards.size(); ++i)
        {
            ss << i;
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
                executable = gpu_container;
                args.push_back(strdup("gpu_container"));

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

        try
        {
            ReleaseRequest params;
            for(auto& card : resp.claimedCards)
                params.gpus.push_back(card.index);

            Request req{params};
            conn.send(req);

            ReleaseResponse resp;
            conn.receive(resp);

            if(resp.errors.empty())
                return 0;

            // Failure, wait a little bit more and try again
            sleep(1);

            conn.send(req);
            resp = {};
            conn.receive(resp);

            if(!resp.errors.empty())
            {
                fprintf(stderr, "Could not release GPUs:\n%s\n", resp.errors.c_str());
                return 1;
            }
        }
        catch(Connection::Error& e)
        {
            fprintf(stderr, "Could not release GPUs. Probably there was a 'gpu' update in the meantime. This is not a problem.\n");
            return 0;
        }
    }
    else
    {
        fprintf(stderr, "Unknown command '%s'. Try --help.\n", command.c_str());
        return 1;
    }

    return 0;
}

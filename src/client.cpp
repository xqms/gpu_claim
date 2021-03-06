// gpu client
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "protocol.h"

#include <boost/program_options.hpp>

#include <cstdio>
#include <iostream>
#include <filesystem>

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

int main(int argc, char** argv)
{
    namespace po = boost::program_options;

    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Help")
        ("num-cards,n", po::value<unsigned int>()->default_value(1)->value_name("N"), "Number of GPUs to claim")
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

    po::notify(vm);

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

            printf("[%lu] %s | %2d%% | %6lu / %6lu MB |",
                i, card.name.c_str(),
                card.computeUsagePercent,
                card.memoryUsage / 1000000UL, card.memoryTotal / 1000000UL
            );

            struct passwd *pws;
            pws = getpwuid(card.reservedByUID);
            if(card.reservedByUID == 0 || !pws)
                printf("%22s |", "free");
            else
            {
                auto idleTime = now - card.lastUsageTime;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(idleTime);

                bool used = std::ranges::any_of(card.processes, [&](auto& proc){
                    return proc.uid == card.reservedByUID;
                });

                if(used)
                    printf("%10s   (running) |", pws->pw_name);
                else
                    printf("%10s (idle %ldmin) |", pws->pw_name, minutes.count());
            }

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
        namespace fs = std::filesystem;

        if(startOfRunArgs == argc)
        {
            fprintf(stderr, "Need command to run.\n");
            return 1;
        }

        std::string executable = argv[startOfRunArgs];

        // If the executable is not absolute, look it up
        if(executable[0] != '/')
        {
            char* path = strdup(getenv("PATH"));

            for(char* dir = strtok(path, ":"); dir; dir = strtok(NULL, ":"))
            {
                fs::path p = fs::path(dir) / executable;
                if(fs::exists(p))
                    executable = p.string();
            }

            free(path);
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
                printf("gpu: Waiting for free cards...\n");
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
            for(int i = startOfRunArgs; i < argc; ++i)
                args.push_back(strdup(argv[i]));
            args.push_back(nullptr);

            if(execv(executable.c_str(), args.data()) != 0)
            {
                perror("Could not execute command");
                std::exit(1);
            }

            // Should not reach here
            std::abort();
        }

        int status = 0;
        if(waitpid(pid, &status, 0) < 0)
        {
            perror("Could not wait for child process");
            std::exit(1);
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

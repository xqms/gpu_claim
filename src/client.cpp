// gpu client
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "protocol.h"

#include <boost/program_options.hpp>

#include <cstdio>
#include <iostream>

#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <pwd.h>

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
    po::store(po::command_line_parser(argc, argv).options(allOptions).positional(p).run(), vm);

    if(vm.count("help"))
    {
        fprintf(stderr, "Usage: gpu <command> [options]\n"
            "Available commands:\n"
            "  gpu status:\n"
            "    List current GPU allocation & status\n"
            "  gpu claim:\n"
            "    Claim one or more GPUs\n"
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
                printf("%24s |", "free");
            else
            {
                auto idleTime = now - card.lastUsageTime;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(idleTime);
                if(minutes.count() == 0)
                    printf("%10s    (running) |", pws->pw_name);
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

    return 0;
}

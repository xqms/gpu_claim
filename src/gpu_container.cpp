// Containerize the child process so that it only sees the specified GPUs
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include <sched.h>
#include <stdio.h>

#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <map>
#include <vector>
#include <string>
#include <sstream>

void usage()
{
    fprintf(stderr, "Usage: gpu_container <device file names...> -- <command> [args]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This helper will hide the mentioned device file names from the command to be executed.\n");
}

pid_t childProcessPID = 0;
pid_t userProcessPID = 0;

std::map<std::string, std::string> readProcessEnvironment(pid_t pid)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "/proc/%d/environ", pid);
    int fd = open(buf, O_RDONLY);
    if(fd < 0)
    {
        perror("Could not open parent process environment");
        return {};
    }

    enum class State
    {
        Key,
        Value
    };
    State state = State::Key;

    std::stringstream currentKey;
    std::stringstream currentValue;
    std::map<std::string, std::string> data;

    while(true)
    {
        int ret = read(fd, buf, sizeof(buf));
        if(ret < 0)
        {
            perror("Could not read from parent process environment");
            close(fd);
            return {};
        }
        if(ret == 0)
            break;

        for(int i = 0; i < ret; ++i)
        {
            char c = buf[i];

            switch(state)
            {
                case State::Key:
                    if(c == '=')
                        state = State::Value;
                    else
                        currentKey << c;
                    break;
                case State::Value:
                    if(c == 0)
                    {
                        if(!currentKey.str().empty())
                            data[currentKey.str()] = currentValue.str();

                        currentKey = {};
                        currentValue = {};
                        state = State::Key;
                    }
                    else
                        currentValue << c;
                    break;
            }
        }
    }

    close(fd);
    return data;
}

int main(int argc, char** argv)
{
    if(argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        usage();
        return 1;
    }

    // Find the -- in the argument list
    int sepIdx = 1;
    for(;; sepIdx++)
    {
        if(sepIdx == argc)
        {
            usage();
            return 1;
        }

        if(strcmp(argv[sepIdx], "--") == 0)
            break;
    }

    // The -- should not be the last argument
    if(sepIdx == argc - 1)
    {
        usage();
        return 1;
    }

    // Request that we get a SIGTERM whenever the parent process dies
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    // ld.so strips useful environment variables like LD_LIBRARY_PATH
    // from the environment of a setuid executable (which we are).
    // So we read the environment of the parent process and restore it in our child.
    // This is not a security problem since our child has the same capabilities
    // as our parent (it's just this process that's elevated).
    auto parentEnv = readProcessEnvironment(getppid());

    // Let's go!
    if(unshare(CLONE_NEWNS | CLONE_NEWPID) != 0)
    {
        perror("Could not create mount / PID namespace");
        return 1;
    }

    // Make private
    if(mount("/", "/", nullptr, MS_PRIVATE | MS_REC, nullptr) != 0)
    {
        perror("Could not make mounts private");
        return 1;
    }

    mkdir("/tmp/select_nvidia", 0755);
    chmod("/tmp/select_nvidia", 0755);

    if(mount("none", "/tmp/select_nvidia", "tmpfs", 0, nullptr) != 0)
    {
        perror("Could not mount tmpfs");
        return 1;
    }

    mkdir("/tmp/select_nvidia/workdir", 0755);
    mkdir("/tmp/select_nvidia/upper", 0755);

    // Create whiteout files -- overlayfs will hide these files
    for(int i = 1; i < sepIdx; ++i)
    {
        auto filename = std::string{"/tmp/select_nvidia/upper/"} + argv[i];
        mknod(filename.c_str(), S_IFCHR | 0666, makedev(0, 0));
    }

    // Bind /dev/pts somewhere else temporarily
    mkdir("/tmp/select_nvidia/pts", 0700);
    if(mount("/dev/pts", "/tmp/select_nvidia/pts", "", MS_MOVE, nullptr) != 0)
    {
        perror("Could not move /dev/pts");
        return 1;
    }

    if(mount("overlay", "/dev", "overlay", 0, "lowerdir=/dev,workdir=/tmp/select_nvidia/workdir,upperdir=/tmp/select_nvidia/upper") != 0)
    {
        perror("Could not create /dev overlay");
        return 1;
    }

    // Move /dev/pts back on top
    if(mount("/tmp/select_nvidia/pts", "/dev/pts", "", MS_MOVE, nullptr) != 0)
    {
        perror("Could not move /dev/pts back");
        return 1;
    }

    // Mount our own /dev/shm
    {
        if(mount("tmpfs", "/dev/shm", "tmpfs", 0, nullptr) != 0)
        {
            perror("Could not mount /dev/shm");
            return 1;
        }
    }

    childProcessPID = fork();
    if(childProcessPID < 0)
    {
        perror("Could not fork()");
        return 1;
    }

    if(childProcessPID == 0)
    {
        // Child process, running inside the PID namespace (with PID 1)

        // Request that we get a SIGTERM whenever the parent process dies
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        // Remount /proc on top, since we are inside a PID namespace
        if(mount("proc", "/proc", "proc", 0, nullptr) != 0)
        {
            perror("Could not mount /proc inside container");
            return 1;
        }

        // Drop privileges
        auto uid = getuid();
        if(setreuid(uid, uid) != 0)
        {
            perror("Could not drop privileges");
            return 1;
        }

        userProcessPID = fork();
        if(userProcessPID < 0)
        {
            perror("Could not fork()");
            return 1;
        }

        if(userProcessPID == 0)
        {
            // Child process, will become the user application

            // Load variables from parent process environment if they are not already present
            for(auto& entry : parentEnv)
            {
                if(getenv(entry.first.c_str()))
                    continue;

                setenv(entry.first.c_str(), entry.second.c_str(), 1);
            }

            if(execvp(argv[sepIdx + 1], argv + sepIdx + 1) != 0)
            {
                perror("Could not execvp");
                return 1;
            }
        }
        else
        {
            // Wait for child processes that exit. If it's our direct child process
            // (i.e. the user app), exit ourselves.

            // Forward SIGINT to child
            signal(SIGINT, [](int){
                if(kill(userProcessPID, SIGINT) != 0)
                    perror("Could not send SIGINT to user process");
            });

            while(true)
            {
                auto child = wait(nullptr);
                if(child < 0)
                {
                    if(errno == EINTR)
                        continue;

                    perror("Could not wait() for childs");
                    return 1;
                }
                if(child == userProcessPID)
                {
                    return 0;
                }
            }
        }
    }
    else
    {
        // Parent process running outside the PID namespace

        // Drop privileges
        auto uid = getuid();
        if(setreuid(uid, uid) != 0)
        {
            perror("Could not drop privileges");
            return 1;
        }

        // Forward SIGINT to child
        signal(SIGINT, [](int){
            if(kill(childProcessPID, SIGINT) != 0)
                perror("Could not send SIGINT to child container process");
        });

        while(true)
        {
            auto child = waitpid(childProcessPID, nullptr, 0);
            if(child < 0)
            {
                if(errno == EINTR)
                    continue;

                perror("Could not waitpid() for child process");
                return 1;
            }

            break;
        }
    }

    return 0;
}

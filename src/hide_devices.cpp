// Hide
#include <sched.h>
#include <stdio.h>

#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <vector>
#include <string>

void usage()
{
    fprintf(stderr, "Usage: hide_devices <device file names...> -- <command> [args]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This helper will hide the mentioned device file names from the command to be executed.\n");
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

    // Let's go!
    if(unshare(CLONE_NEWNS) != 0)
    {
        perror("Could not create mount namespace");
        return 1;
    }

    // Make private
    if(mount("/", "/", nullptr, MS_PRIVATE | MS_REC, nullptr) != 0)
    {
        perror("Could not make mounts private");
        return 1;
    }

    mkdir("/tmp/select_nvidia", 0700);

    if(mount("none", "/tmp/select_nvidia", "tmpfs", 0, nullptr) != 0)
    {
        perror("Could not mount tmpfs");
        return 1;
    }

    // Create whiteout files -- overlayfs will hide these files
    for(int i = 1; i < sepIdx; ++i)
    {
        auto filename = std::string{"/tmp/select_nvidia/"} + argv[i];
        mknod(filename.c_str(), S_IFCHR | 0666, makedev(0, 0));
    }

    if(mount("overlay", "/dev", "overlay", 0, "lowerdir=/tmp/select_nvidia:/dev") != 0)
    {
        perror("Could not create /dev overlay");
        return 1;
    }

    auto uid = getuid();
    if(setreuid(uid, uid) != 0)
    {
        perror("Could not drop privileges");
        return 1;
    }

    if(execvp(argv[sepIdx + 1], argv + sepIdx + 1) != 0)
    {
        perror("Could not execvp");
        return 1;
    }

    return 0;
}

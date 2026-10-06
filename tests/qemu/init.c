// SPDX-License-Identifier: GPL-2.0-only
/*
 * /init for the Track B guest.
 *
 * The guest has no disk image. It boots the host's kernel with this program
 * as its whole initramfs, mounts the host's root file system read-only over
 * 9p, and runs the test script from there as root. That gives the tests the
 * host's Mesa, Python and build directory without downloading anything.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void must(int rc, const char *what)
{
    if (rc < 0) {
        perror(what);
        sync();
        reboot(RB_POWER_OFF);
    }
}

static void load_modules(void)
{
    FILE *f = fopen("/modules/order", "r");
    char name[256], path[300];

    while (f && fgets(name, sizeof(name), f)) {
        int fd;

        name[strcspn(name, "\n")] = '\0';
        snprintf(path, sizeof(path), "/modules/%s", name);
        fd = open(path, O_RDONLY);
        if (fd < 0 || syscall(SYS_finit_module, fd, "", 0) < 0)
            fprintf(stderr, "init: cannot load %s\n", name);
        if (fd >= 0)
            close(fd);
    }
    if (f)
        fclose(f);
}

int main(void)
{
    static char *const argv[] = { "/bin/sh", "/mnt/run.sh", NULL };
    static char *const envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin", "HOME=/tmp", "XDG_RUNTIME_DIR=/run",
        "XDG_CACHE_HOME=/tmp/cache", "TERM=dumb", "PYTHONDONTWRITEBYTECODE=1", NULL,
    };
    int status = 0;
    pid_t pid;
    FILE *f;

    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/newroot", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    load_modules();

    must(mount("hostroot", "/newroot", "9p", MS_RDONLY, "trans=virtio,version=9p2000.L,msize=262144,cache=loose"),
         "mounting the host root");
    must(mount("devtmpfs", "/newroot/dev", "devtmpfs", 0, NULL), "mounting /dev");
    must(mount("proc", "/newroot/proc", "proc", 0, NULL), "mounting /proc");
    must(mount("sysfs", "/newroot/sys", "sysfs", 0, NULL), "mounting /sys");
    must(mount("cgroup2", "/newroot/sys/fs/cgroup", "cgroup2", 0, NULL), "mounting cgroup2");
    must(mount("tmpfs", "/newroot/tmp", "tmpfs", 0, NULL), "mounting /tmp");
    must(mount("tmpfs", "/newroot/run", "tmpfs", 0, NULL), "mounting /run");
    must(mount("work", "/newroot/mnt", "9p", 0, "trans=virtio,version=9p2000.L,msize=262144"), "mounting the work share");
    mkdir("/newroot/dev/shm", 01777);
    mount("tmpfs", "/newroot/dev/shm", "tmpfs", 0, NULL);
    must(chroot("/newroot"), "chroot");
    must(chdir("/"), "chdir");

    pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, envp);
        perror("init: exec");
        _exit(127);
    }
    waitpid(pid, &status, 0);

    f = fopen("/mnt/exit-code", "w");
    if (f) {
        fprintf(f, "%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128);
        fclose(f);
    }
    sync();
    reboot(RB_POWER_OFF);
    return 0;
}

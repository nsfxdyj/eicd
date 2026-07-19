/*
 * daemonize.c - double-fork daemonisation and PID file locking.
 */
#include "daemonize.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int daemonize_now(void)
{
    pid_t pid;
    int   fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);                       /* parent: child continues      */

    if (setsid() < 0)                   /* new session, no ctrl tty     */
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);                       /* first child: grandchild runs */

    umask(027);

    /* keep the CWD so relative config paths keep working after start */
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
    return 0;
}

/*
 * Open (creating if needed) the PID file and hold an exclusive flock on
 * it for the process lifetime. flock is released by the kernel even on
 * SIGKILL, unlike a plain "does the file exist" check.
 */
int pidfile_acquire(const char *path)
{
    int  fd;
    char buf[16];
    int  len;

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;                      /* another instance holds it    */
    }

    if (ftruncate(fd, 0) != 0) {
        close(fd);
        return -1;
    }
    len = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (write(fd, buf, (size_t)len) != len) {
        close(fd);
        return -1;
    }
    return fd;                          /* intentionally left open      */
}

void pidfile_release(const char *path, int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
    if (path != NULL)
        unlink(path);
}

/*
 * daemonize.h - classic POSIX daemonisation and PID file management.
 *
 * daemonize_now() performs the double-fork/setsid dance and detaches
 * from the controlling terminal. pidfile_acquire() takes an exclusive
 * flock on the PID file so a second instance refuses to start; the lock
 * is held for the process lifetime and released by pidfile_release()
 * (or automatically by the kernel on abnormal exit).
 */
#ifndef EICD_DAEMONIZE_H
#define EICD_DAEMONIZE_H

int  daemonize_now(void);                       /* 0 ok, -1 error        */
int  pidfile_acquire(const char *path);         /* fd >= 0, -1 error     */
void pidfile_release(const char *path, int fd);

#endif /* EICD_DAEMONIZE_H */

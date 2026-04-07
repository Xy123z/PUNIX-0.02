#include "../include/unistd.h"
#include "../include/punix_def.h"
#include "../include/errno.h"

// External assembly wrappers from syscalls.asm
extern int sys_read(int fd, void* buf, size_t count);
extern int sys_write(int fd, const void* buf, size_t count);
extern int sys_open(const char* path, int flags);
extern int sys_close(int fd);
extern int sys_getdents(const char* path, void* buf, int count);
extern int sys_chdir(const char* path);
extern int sys_getcwd(char* buf, size_t size);
extern int sys_mkdir(const char* path);
extern int sys_rmdir(const char* path);
extern int sys_unlink(const char* path);
extern int sys_lseek(int fd, int offset, int whence);
extern int sys_exit(int status);
extern int sys_fork(void);
extern int sys_exec(const char* path, char** argv, char** envp);
extern int sys_wait(int pid, int* status, int options);
extern int sys_kill(int pid);
extern void sys_sleep(uint32_t ticks);
extern uint32_t sys_getpid(void);
extern uint32_t sys_getuid(void);
extern int sys_setuid(uint32_t uid);
extern uint32_t sys_getgid(void);
extern int sys_setgid(uint32_t gid);
extern int sys_tcgetattr(int fd, void* termios_p);
extern int sys_tcsetattr(int fd, int optional_actions, const void* termios_p);
extern int sys_ioctl(int fd, uint32_t request, void* argp);
extern int sys_sigaction(int sig, void* handler);
extern int sys_getpgrp(void);
extern int sys_setpgid(uint32_t pid, uint32_t pgid);
extern int sys_getsid(uint32_t pid);
extern int sys_setsid(void);
extern int sys_authenticate(const char* password);
extern void sys_sync(void);
extern uint32_t sys_get_ticks(void);
extern int sys_dup2(int oldfd, int newfd);
extern int sys_pipe(int pipefd[2]);
extern int sys_stat(const char* path, void* buf);
extern int sys_chmod(const char* path, uint32_t mode);
extern int sys_ftruncate(int fd, uint32_t length);
extern int sys_mount(const char* source, const char* target, const char* filesystemtype);

// Helper: decode Linux-style negative-errno return from kernel.
// If ret > 0x80000000 it's a negative-signed value encoding an errno number.
// Sets errno and returns -1. Otherwise returns ret unchanged.
static inline int __syscall_ret(unsigned int ret) {
    if (ret > 0x80000000U) {
        errno = -(int)ret;
        return -1;
    }
    return (int)ret;
}

int read(int fd, void* buf, size_t count) {
    return __syscall_ret((unsigned int)sys_read(fd, buf, count));
}

int write(int fd, const void* buf, size_t count) {
    return __syscall_ret((unsigned int)sys_write(fd, buf, count));
}

int open(const char* path, int flags, ...) {
    return __syscall_ret((unsigned int)sys_open(path, flags));
}

int close(int fd) {
    return __syscall_ret((unsigned int)sys_close(fd));
}

int lseek(int fd, int offset, int whence) {
    return __syscall_ret((unsigned int)sys_lseek(fd, offset, whence));
}

#include <sys/time.h>
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv) {
        tv->tv_sec = time(NULL);
        tv->tv_usec = 0;
    }
    return 0;
}

int unlink(const char* path) {
    return __syscall_ret((unsigned int)sys_unlink(path));
}

int dup2(int oldfd, int newfd) {
    return __syscall_ret((unsigned int)sys_dup2(oldfd, newfd));
}

int pipe(int pipefd[2]) {
    return __syscall_ret((unsigned int)sys_pipe(pipefd));
}

int stat(const char* path, struct stat* buf) {
    return __syscall_ret((unsigned int)sys_stat(path, (void*)buf));
}

int fstat(int fd, struct stat* buf) {
    // PUNIX might not have sys_fstat yet, so we'll just return error or dummy
    return -1;
}

int lstat(const char* path, struct stat* buf) {
    return __syscall_ret((unsigned int)sys_stat(path, (void*)buf));
}

int chmod(const char* path, mode_t mode) {
    return __syscall_ret((unsigned int)sys_chmod(path, (uint32_t)mode));
}

int fchmod(int fd, mode_t mode) {
    return -1;
}

int ftruncate(int fd, uint32_t length) {
    return __syscall_ret((unsigned int)sys_ftruncate(fd, length));
}

#include <time.h>
extern time_t sys_time(time_t* tloc);
time_t time(time_t* tloc) {
    return sys_time(tloc);
}

int chdir(const char* path) {
    return __syscall_ret((unsigned int)sys_chdir(path));
}

char* getcwd(char* buf, size_t size) {
    if (sys_getcwd(buf, size) == 0) return buf;
    return NULL;
}

int mkdir(const char* path) {
    return __syscall_ret((unsigned int)sys_mkdir(path));
}

int rmdir(const char* path) {
    return __syscall_ret((unsigned int)sys_rmdir(path));
}

int mount(const char* source, const char* target, const char* filesystemtype) {
    return __syscall_ret((unsigned int)sys_mount(source, target, filesystemtype));
}

int fork(void) {
    return __syscall_ret((unsigned int)sys_fork());
}

int execve(const char* path, char** argv, char** envp) {
    return __syscall_ret((unsigned int)sys_exec(path, argv, envp));
}

int exec(const char* path, char** argv) {
    return execve(path, argv, environ);
}

int waitpid(int pid, int* status, int options) {
    return __syscall_ret((unsigned int)sys_wait(pid, status, options));
}

int wait(int* status) {
    return waitpid(-1, status, 0);
}

int kill(int pid) {
    return sys_kill(pid);
}

unsigned int sleep(unsigned int seconds) {
    // Assuming 100 ticks per second for now, or just pass as is if ticks
    sys_sleep(seconds * 100); 
    return 0;
}

uint32_t getpid(void) {
    return sys_getpid();
}

uint32_t getuid(void) {
    return sys_getuid();
}

int setuid(uint32_t uid) {
    return sys_setuid(uid);
}

uint32_t getgid(void) {
    return sys_getgid();
}

int setgid(uint32_t gid) {
    return sys_setgid(gid);
}

void sync(void) {
    sys_sync();
}

struct termios;
int tcgetattr(int fd, struct termios* termios_p) {
    return __syscall_ret((unsigned int)sys_tcgetattr(fd, (void*)termios_p));
}

int tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    return __syscall_ret((unsigned int)sys_tcsetattr(fd, optional_actions, (void*)termios_p));
}

int ioctl(int fd, uint32_t request, void* argp) {
    return __syscall_ret((unsigned int)sys_ioctl(fd, request, argp));
}

int sigaction(int sig, void* handler) {
    return __syscall_ret((unsigned int)sys_sigaction(sig, handler));
}

#include <signal.h>
sighandler_t signal(int signum, sighandler_t handler) {
    if (sys_sigaction(signum, (void*)handler) == 0) return handler;
    return (sighandler_t)-1;
}

int isatty(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

extern int sys_sbrk(int increment);
void* sbrk(intptr_t inc) {
    void* ret = (void*)sys_sbrk(inc);
    return ret;
}

int getpgrp(void) {
    return sys_getpgrp();
}

int setpgid(uint32_t pid, uint32_t pgid) {
    return sys_setpgid(pid, pgid);
}

int getsid(uint32_t pid) {
    return sys_getsid(pid);
}

int setsid(void) {
    return sys_setsid();
}

int authenticate(const char* password) {
    return sys_authenticate(password);
}

uint32_t get_ticks(void) {
    return sys_get_ticks();
}

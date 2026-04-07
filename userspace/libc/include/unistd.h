#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include <stdint.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// ─── Open flags ──────────────────────────────────────────────────────────
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// File I/O
int      read(int fd, void* buf, size_t count);
int      write(int fd, const void* buf, size_t count);
int      open(const char* path, int flags, ...);
int      close(int fd);
int      lseek(int fd, int offset, int whence);
int      unlink(const char* path);
int      dup2(int oldfd, int newfd);
int      pipe(int pipefd[2]);
#include <sys/stat.h>
int      stat(const char* path, struct stat* buf);
int      fstat(int fd, struct stat* buf);
int      lstat(const char* path, struct stat* buf);
int      chmod(const char* path, mode_t mode);
int      fchmod(int fd, mode_t mode);
int      ftruncate(int fd, uint32_t length);
int      isatty(int fd);
void*    sbrk(intptr_t inc);
struct termios;
int      tcgetattr(int fd, struct termios* termios_p);
int      tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
int      ioctl(int fd, uint32_t request, void* argp);
int      sigaction(int sig, void* handler);

// Directory/File system
int      chdir(const char* path);
char*    getcwd(char* buf, size_t size);
int      mkdir(const char* path);
int      mount(const char* source, const char* target, const char* filesystemtype);
int      rmdir(const char* path);

// Process management
int      fork(void);
int      exec(const char* path, char** argv);
int      execve(const char* path, char** argv, char** envp);
extern char** environ;
void     exit(int status);
int      waitpid(int pid, int* status, int options);
int      wait(int* status);
int      kill(int pid);
unsigned int sleep(unsigned int seconds);
uint32_t getpid(void);
uint32_t getuid(void);
int      setuid(uint32_t uid);
uint32_t getgid(void);
int      setgid(uint32_t gid);
int      getpgrp(void);
int      setpgid(uint32_t pid, uint32_t pgid);
int      getsid(uint32_t pid);
int      setsid(void);

// Misc
void     sync(void);
uint32_t get_ticks(void);
int      authenticate(const char* password);

#endif

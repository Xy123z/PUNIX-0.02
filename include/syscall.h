// include/syscall.h - System Call Interface (Unix-conforming)

#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "task.h"
#include "tty.h"

extern uint32_t kernel_esp_saved;
#define MAX_PATH 256

// ─── Stat structure ───────────────────────────────────────────────────────
typedef struct {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint8_t  st_type;
} struct_stat_t;

// ─── dirent ──────────────────────────────────────────────────────────────
struct dirent {
    uint32_t d_ino;
    uint8_t  d_type;
    char     d_name[64];
};

// ─── Open flags (also in task.h, kept here for userspace include compat) ─
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

// ─── Syscall numbers (Aligned with Linux i386) ──────────────────────────
#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_OPEN         5
#define SYS_CLOSE        6
#define SYS_WAIT         7
#define SYS_CREATE_FILE  8
#define SYS_UNLINK       10
#define SYS_EXEC         11
#define SYS_CHDIR        12
#define SYS_TIME         13
#define SYS_CHMOD        15
#define SYS_STAT         18
#define SYS_LSEEK        19
#define SYS_GETPID       20
#define SYS_MOUNT        21
#define SYS_SETUID       23
#define SYS_GETUID       24
#define SYS_SYNC         36
#define SYS_KILL         37
#define SYS_MKDIR        39
#define SYS_RMDIR        40
#define SYS_PIPE         42
#define SYS_SBRK         45
#define SYS_SETGID       46
#define SYS_GETGID       47
#define SYS_IOCTL        54
#define SYS_SETPGID      57
#define SYS_DUP2         63
#define SYS_GETPGRP      65
#define SYS_SETSID       66
#define SYS_SIGACTION    67
#define SYS_FTRUNCATE    93
#define SYS_GETDENTS     141
#define SYS_GETCWD       183

// PUNIX-specific extensions (200+)
#define SYS_GET_DISK_STATS  201
#define SYS_GET_CACHE_STATS 202
#define SYS_CHUSER       203
#define SYS_CHPASS       204
#define SYS_AUTHENTICATE 205
#define SYS_SHUTDOWN     206
#define SYS_RESTART      207
#define SYS_GET_MEM_STATS 208
#define SYS_GET_PROCS    209
#define SYS_SLEEP        210
#define SYS_GET_TICKS    211
#define SYS_KBHIT        212
#define SYS_GET_USERNAME 213
#define SYS_GETSID       214
#define SYS_SET_USERNAME 215
#define SYS_REGISTER     216
#define SYS_TCGETATTR    217
#define SYS_TCSETATTR    218
#define SYS_CLEAR_SCREEN 219

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ─── Kernel-side functions ────────────────────────────────────────────────
void      syscall_init(void);
void      syscall_set_cwd(uint32_t id);
void      syscall_free_fd(task_t* task, int fd);
void      syscall_close_all(task_t* task);
uint32_t  syscall_handler(registers_t* regs);
extern void syscall_interrupt_wrapper(void);

#endif // SYSCALL_H

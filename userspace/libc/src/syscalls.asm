[bits 32]

section .text

%macro SYSCALL_0 2
global %1
%1:
    mov eax, %2
    int 0x80
    ret
%endmacro

%macro SYSCALL_1 2
global %1
%1:
    push ebx
    mov eax, %2
    mov ebx, [esp + 8]
    int 0x80
    pop ebx
    ret
%endmacro

%macro SYSCALL_2 2
global %1
%1:
    push ebx
    mov eax, %2
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    int 0x80
    pop ebx
    ret
%endmacro

%macro SYSCALL_3 2
global %1
%1:
    push ebx
    mov eax, %2
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov edx, [esp + 16]
    int 0x80
    pop ebx
    ret
%endmacro

%macro SYSCALL_4 2
global %1
%1:
    push ebx
    push esi
    mov eax, %2
    mov ebx, [esp + 12]
    mov ecx, [esp + 16]
    mov edx, [esp + 20]
    mov esi, [esp + 24]
    int 0x80
    pop esi
    pop ebx
    ret
%endmacro

SYSCALL_3 sys_read, 3
SYSCALL_3 sys_write, 4
SYSCALL_2 sys_open, 5
SYSCALL_1 sys_close, 6
SYSCALL_3 sys_getdents, 141
SYSCALL_1 sys_chdir, 12
SYSCALL_2 sys_getcwd, 183
SYSCALL_1 sys_mkdir, 39
SYSCALL_1 sys_rmdir, 40
SYSCALL_1 sys_unlink, 10
SYSCALL_2 sys_stat, 18
SYSCALL_1 sys_exit, 1
SYSCALL_0 sys_getpid, 20
SYSCALL_1 sys_sbrk, 45
SYSCALL_1 sys_print, 255 ; Unused/Deprecated
SYSCALL_1 sys_create_file, 8
SYSCALL_1 sys_putchar, 254 ; Unused/Deprecated
SYSCALL_2 sys_print_colored, 253 ; Unused/Deprecated
SYSCALL_0 sys_clear_screen, 252 ; Unused/Deprecated
SYSCALL_3 sys_get_disk_stats, 201
SYSCALL_3 sys_get_cache_stats, 202
SYSCALL_0 sys_sync, 36
SYSCALL_1 sys_chuser, 203
SYSCALL_1 sys_chpass, 204
SYSCALL_0 sys_getuid, 24
SYSCALL_1 sys_setuid, 23
SYSCALL_1 sys_authenticate, 205
SYSCALL_0 sys_shutdown, 206
SYSCALL_0 sys_restart, 207
SYSCALL_3 sys_get_mem_stats, 208
SYSCALL_3 sys_exec, 11
SYSCALL_0 sys_fork, 2
SYSCALL_2 sys_get_procs, 209
SYSCALL_1 sys_kill, 37
SYSCALL_1 sys_sleep, 210
SYSCALL_0 sys_get_ticks, 211
SYSCALL_0 sys_kbhit, 212
SYSCALL_3 sys_wait, 7
SYSCALL_2 sys_get_username, 213
SYSCALL_2 sys_chmod, 15
SYSCALL_2 sys_dup2, 63
SYSCALL_1 sys_pipe, 42
SYSCALL_0 sys_getgid, 47
SYSCALL_1 sys_setgid, 46
SYSCALL_2 sys_tcgetattr, 217
SYSCALL_3 sys_tcsetattr, 218
SYSCALL_3 sys_ioctl, 54
SYSCALL_2 sys_sigaction, 67
SYSCALL_0 sys_getpgrp, 65
SYSCALL_2 sys_setpgid, 57
SYSCALL_1 sys_getsid, 214
SYSCALL_0 sys_setsid, 66
SYSCALL_1 sys_set_username, 215
SYSCALL_2 sys_register, 216
SYSCALL_2 sys_ftruncate, 93
SYSCALL_3 sys_lseek, 19
SYSCALL_1 sys_time, 13
SYSCALL_3 sys_mount, 21

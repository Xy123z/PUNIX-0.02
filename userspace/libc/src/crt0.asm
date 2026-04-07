[bits 32]

section .text
global _start
extern main
extern exit

_start:
    extern environ
    ; loader.c puts argc, argv, envp on the stack
    ; [esp]   = argc
    ; [esp+4] = argv
    ; [esp+8] = envp
    
    ; Set environ global
    mov eax, [esp + 8]
    mov [environ], eax

    ; Push them in reverse order for main(argc, argv, envp)
    push dword [esp + 8] ; envp
    push dword [esp + 8] ; argv (was +4, +4 more due to push)
    push dword [esp + 8] ; argc (was +0, +8 more due to two pushes)
    
    call main
    
    ; Exit with main's return value
    push eax
    call exit
    
    ; Should never reach here
    hlt

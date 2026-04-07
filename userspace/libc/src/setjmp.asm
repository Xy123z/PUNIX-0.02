; userspace/libc/src/setjmp.asm
[bits 32]
section .text
global setjmp
global longjmp

setjmp:
    mov eax, [esp + 4] ; jmp_buf pointer
    mov [eax + 0], ebp
    mov [eax + 4], esp
    mov [eax + 8], ebx
    mov [eax + 12], esi
    mov [eax + 16], edi
    
    ; Get return address from stack and save it as eip
    mov edx, [esp]
    mov [eax + 20], edx
    
    xor eax, eax       ; return 0 for setjmp
    ret

longjmp:
    mov edx, [esp + 4] ; jmp_buf pointer
    mov eax, [esp + 8] ; return value for setjmp
    
    ; If val is 0, setjmp must return 1
    test eax, eax
    jnz .val_ok
    inc eax
.val_ok:
    ; Restore registers
    mov ebp, [edx + 0]
    mov esp, [edx + 4]
    mov ebx, [edx + 8]
    mov esi, [edx + 12]
    mov edi, [edx + 16]
    
    ; Get saved eip and put it on top of restored stack for ret
    mov ecx, [edx + 20]
    mov [esp], ecx
    
    ret

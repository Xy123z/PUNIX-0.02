#ifndef SETJMP_H
#define SETJMP_H

// jmp_buf for x86 32-bit: ebp, esp, ebx, esi, edi, eip
typedef int jmp_buf[6];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif

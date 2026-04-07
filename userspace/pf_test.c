#include<stdio.h>
#include<stdint.h>
int main(){
uint32_t* p = (uint32_t*)0xB8000;
uintptr_t x = (uintptr_t)&p;
printf("%x", (void*)x);
}

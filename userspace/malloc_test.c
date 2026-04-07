#include<stdlib.h>
#include<stdio.h>
#include<stdint.h>
int main(){
    char c = 'c';
char* p = malloc(1);
printf("%x\n",(uint32_t)p);
return 0;
}

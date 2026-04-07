#include<stdio.h>
#include<stdlib.h>
int main(){
char* p = malloc(10);
for (int i = 0; i < 10; i++) p[i] = 'A' + i;
for (int i = 0; i < 10; i++) printf("%c", p[i]);
return 0;
}

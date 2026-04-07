#include<stdio.h>
#include<stdlib.h>
#include<string.h>
int main(){
void *p = malloc(100);
memset(p, 'A', 100);
p = realloc(p, 200);
memset(p, 'B', 200);
return 0;

}

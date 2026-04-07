#include<stdlib.h>
#include<stdio.h>
int main(){
    const char* home = getenv("HOME");
    const char* user = getenv("USER");
    const char* path = getenv("PATH");
    const char* gecos = getenv("GECOS");
     const char* shell = getenv("SHELL");
      const char* term = getenv("TERM");
    printf("%s\n%s\n%s\n%s\n%s\n%s\n",home,user,path,gecos,shell,term);
}

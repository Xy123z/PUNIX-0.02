#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char* argv[], char* envp[]) {
    printf("Environment Test Program\n");
    printf("------------------------\n");
    
    printf("argc: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }
    
    printf("\nEnvironment variables via envp:\n");
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            printf("%s\n", envp[i]);
        }
    } else {
        printf("(envp is NULL)\n");
    }
    
    printf("\nEnvironment variables via getenv:\n");
    const char* user = getenv("USER");
    const char* home = getenv("HOME");
    const char* path = getenv("PATH");
    
    printf("USER: %s\n", user ? user : "(null)");
    printf("HOME: %s\n", home ? home : "(null)");
    printf("PATH: %s\n", path ? path : "(null)");
    
    return 0;
}

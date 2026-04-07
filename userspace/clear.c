#include <stdio.h>
#include <unistd.h>

int main() {
    printf("\x1b[2J\x1b[H"); 
    return 0;
}

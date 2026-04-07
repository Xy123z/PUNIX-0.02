#include <stdio.h>
#include <string.h>

int main() {
    printf("Testing sscanf...\n");
    const char* input = "123 hello 0xABC";
    int i;
    char s[64];
    unsigned int x;
    
    int count = sscanf(input, "%d %s %x", &i, s, &x);
    printf("Parsed %d items: i=%d, s=%s, x=0x%x\n", count, i, s, x);
    
    if (count == 3 && i == 123 && strcmp(s, "hello") == 0 && x == 0xABC) {
        printf("sscanf test PASSED\n");
    } else {
        printf("sscanf test FAILED\n");
    }

    printf("\nTesting sscanf with literal matching...\n");
    const char* input2 = "Value: 42";
    int val;
    count = sscanf(input2, "Value: %d", &val);
    printf("Parsed %d items: val=%d\n", count, val);
    if (count == 1 && val == 42) {
        printf("sscanf literal test PASSED\n");
    } else {
        printf("sscanf literal test FAILED\n");
    }

    return 0;
}

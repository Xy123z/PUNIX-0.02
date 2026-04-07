#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void* test_sbrk_mem[100000];
int sbrk_idx = 0;

struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_oc;
};

int main() {
    struct erow *row = NULL;
    printf("Starting kilo mock...\n");
    int i;
    for(i=0; i<100; i++) {
        row = realloc(row, sizeof(struct erow)*(i+1));
        if (!row) { printf("realloc failed\n"); return 1; }
        row[i].idx = i;
        row[i].size = 50;
        row[i].chars = malloc(51);
        row[i].render = malloc(51);
        row[i].hl = realloc(NULL, 0); // Kilo does this 
    }
    printf("Success!\n");
    return 0;
}

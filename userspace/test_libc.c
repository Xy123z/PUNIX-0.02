#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

jmp_buf env;

void test_longjmp() {
    printf("  In test_longjmp, calling longjmp...\n");
    longjmp(env, 42);
}

int main(int argc, char** argv) {
    printf("PUNIX Libc Verification Tool\n");
    printf("============================\n");

    // 1. Test argc/argv
    printf("1. Testing argc/argv:\n");
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }

    // 2. Test getenv
    printf("\n2. Testing getenv:\n");
    printf("  PATH = %s\n", getenv("PATH"));
    printf("  TCC_HOME = %s\n", getenv("TCC_HOME"));
    printf("  NON_EXISTENT = %s\n", getenv("NON_EXISTENT"));

    // 3. Test setjmp/longjmp
    printf("\n3. Testing setjmp/longjmp:\n");
    int val = setjmp(env);
    if (val == 0) {
        printf("  setjmp returned 0, calling test_longjmp...\n");
        test_longjmp();
    } else {
        printf("  Returned from longjmp! val = %d (expected 42)\n", val);
    }

    // 4. Test lseek/fseek/fread/fwrite
    printf("\n4. Testing file I/O (lseek/fseek):\n");
    FILE* f = fopen("test.tmp", "w+");
    if (!f) {
        perror("fopen failed");
        return 1;
    }
    const char* data = "Hello PUNIX!";
    fwrite(data, 1, strlen(data), f);
    printf("  Wrote '%s' to test.tmp\n", data);

    fseek(f, 6, SEEK_SET);
    printf("  fseek to 6, ftell = %ld\n", ftell(f));
    
    char buf[16];
    memset(buf, 0, sizeof(buf));
    fread(buf, 1, 5, f);
    printf("  Read 5 bytes: '%s' (expected 'PUNIX')\n", buf);

    fclose(f);
    unlink("test.tmp");

    printf("\nVerification Complete!\n");
    return 0;
}

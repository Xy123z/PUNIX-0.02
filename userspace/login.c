#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <punix.h>
#include <sys/stat.h>

#define PASSWD_FILE "/etc/passwd"

typedef struct {
    char user[32];
    char pass[64];
    int  uid;
    int  gid;
    char gecos[64];
    char home[64];
    char shell[64];
} passwd_entry_t;

void read_password(char* buf, int max) {
    termios_t t;
    tcgetattr(0, &t);
    uint32_t old_lflag = t.c_lflag;
    t.c_lflag &= ~ECHO;
    tcsetattr(0, TCSANOW, &t);

    if (fgets(buf, max, stdin)) {
        char* nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }

    t.c_lflag = old_lflag;
    tcsetattr(0, TCSANOW, &t);
    printf("\n");
}

int parse_passwd_line(char* line, passwd_entry_t* entry) {
    char* s = line;
    char* next;

    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; strcpy(entry->user,  s); s = next + 1;
    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; strcpy(entry->pass,  s); s = next + 1;
    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; entry->uid = atoi(s);    s = next + 1;
    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; entry->gid = atoi(s);    s = next + 1;
    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; strcpy(entry->gecos, s); s = next + 1;
    next = strchr(s, ':'); if (!next) return 0; *next = '\0'; strcpy(entry->home,  s); s = next + 1;
    strcpy(entry->shell, s);
    char* nl = strchr(entry->shell, '\n'); if (nl) *nl = '\0';
    return 1;
}

int find_user(const char* username, passwd_entry_t* entry) {
    FILE* f = fopen(PASSWD_FILE, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (parse_passwd_line(line, entry)) {
            if (strcmp(entry->user, username) == 0) {
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

void add_user(passwd_entry_t* entry) {
    FILE* f = fopen(PASSWD_FILE, "a");
    if (!f) return;
    fprintf(f, "%s:%s:%d:%d:%s:%s:%s\n",
            entry->user, entry->pass,
            entry->uid,  entry->gid,
            entry->gecos, entry->home, entry->shell);
    fclose(f);
}

int register_user(const char* username, passwd_entry_t* entry) {
    printf("\n=== User Registration ===\n");
    printf("Registering new user '%s'\n", username);

    char pass[64], conf[64];
    while (1) {
        printf("Password: ");        read_password(pass, sizeof(pass));
        printf("Confirm password: "); read_password(conf, sizeof(conf));
        if (strcmp(pass, conf) == 0) break;
        printf("Passwords do not match. Try again.\n");
    }

    strcpy(entry->user, username);
    strcpy(entry->pass, pass);
    sprintf(entry->gecos, "New user %s", username);
    sprintf(entry->home,  "/home/%s", username);
    strcpy(entry->shell, "/bin/sh");

    // Assign a uid one above the current highest
    entry->uid = 1000;
    FILE* f = fopen(PASSWD_FILE, "r");
    if (f) {
        char line[256];
        passwd_entry_t tmp;
        while (fgets(line, sizeof(line), f)) {
            if (parse_passwd_line(line, &tmp)) {
                if (tmp.uid >= entry->uid)
                    entry->uid = tmp.uid + 1;
            }
        }
        fclose(f);
    }
    entry->gid = entry->uid;

    add_user(entry);
    printf("User '%s' registered successfully.\n\n", username);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("login: missing username\n");
        return 1;
    }

    char* username = argv[1];
    
    // Sanitize trailing whitespace/control characters (like \r or space)
    int len = strlen(username);
    while (len > 0 && username[len - 1] <= ' ') {
        username[len - 1] = '\0';
        len--;
    }
    passwd_entry_t entry;

    // --- Step 1: find or register the user ---
    if (!find_user(username, &entry)) {
        register_user(username, &entry);
    }

    // --- Step 2: authenticate ---
    char password[64];
    printf("Password: ");
    read_password(password, sizeof(password));

    if (strcmp(password, entry.pass) != 0) {
        printf("Login incorrect\n");
        sleep(1);
        return 1;
    }

    // --- Step 3: set up the session ---
    sys_set_username(entry.user);
    setuid(entry.uid);
    setgid(entry.gid);

    setenv("USER",  entry.user,  1);
    setenv("HOME",  entry.home,  1);
    setenv("SHELL", entry.shell, 1);
    setenv("GECOS", entry.gecos, 1);
    setenv("PATH",  "/bin:/usr/bin:/sbin:/usr/sbin", 1);
    setenv("TERM",  "xterm", 1);

    // --- Step 4: ensure home directory exists and cd into it ---
    struct_stat_t st;
    if (sys_stat(entry.home, &st) != 0) {
        if (mkdir(entry.home) != 0) {
            printf("login: warning: could not create %s, falling back to /\n", entry.home);
            strcpy(entry.home, "/");
        }
    }

    if (chdir(entry.home) != 0) {
        printf("login: chdir to %s failed, falling back to /\n", entry.home);
        chdir("/");
        setenv("PWD", "/", 1);
    } else {
        setenv("PWD", entry.home, 1);
    }

    // --- Step 5: launch the shell ---
    printf("Welcome to PUNIX, %s!\n", entry.user);

    char* shell_argv[] = { entry.shell, NULL };
    execve(entry.shell, shell_argv, environ);

    printf("login: exec %s failed\n", entry.shell);
    return 1;
}

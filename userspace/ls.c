#include <punix.h>
#include <stdio.h>
#include <string.h>

void format_time(uint32_t timestamp, char* buf) {
    if (timestamp == 0) {
        strcpy(buf, "Jan  1 00:00");
        return;
    }

    uint32_t seconds = timestamp;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    uint32_t days = hours / 24;
    hours %= 24;

    uint32_t year = 1970;
    while (1) {
        uint32_t days_in_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
        if (days < days_in_year) break;
        days -= days_in_year;
        year++;
    }

    uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) month_days[1] = 29;

    uint32_t month = 0;
    while (days >= month_days[month]) {
        days -= month_days[month];
        month++;
    }

    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    sprintf(buf, "%s %2d %02d:%02d", months[month], days + 1, hours, minutes);
}

void print_perms(uint32_t mode, uint8_t type) {
    char perm[11];
    perm[0] = (type == FS_TYPE_DIRECTORY) ? 'd' : '-';
    if (type == FS_TYPE_CHARDEV) perm[0] = 'c';
    perm[1] = (mode & 0400) ? 'r' : '-';
    perm[2] = (mode & 0200) ? 'w' : '-';
    perm[3] = (mode & 0100) ? 'x' : '-';
    perm[4] = (mode & 0040) ? 'r' : '-';
    perm[5] = (mode & 0020) ? 'w' : '-';
    perm[6] = (mode & 0010) ? 'x' : '-';
    perm[7] = (mode & 0004) ? 'r' : '-';
    perm[8] = (mode & 0002) ? 'w' : '-';
    perm[9] = (mode & 0001) ? 'x' : '-';
    perm[10] = '\0';
    printf("%s ", perm);
}

int main(int argc, char** argv) {
    char path[256];
    if (argc > 1) {
        strcpy(path, argv[1]);
    } else {
        strcpy(path, ".");
    }

    struct dirent entries[64];
    int count = sys_getdents(path, (void*)entries, 64);

    if (count < 0) {
        printf("ls: cannot access '%s'\n", path);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        char full_path[512];
        if (strcmp(path, "/") == 0) {
            sprintf(full_path, "/%s", entries[i].d_name);
        } else {
            sprintf(full_path, "%s/%s", path, entries[i].d_name);
        }

        struct_stat_t st;
        if (sys_stat(full_path, &st) == 0) {
            char time_buf[20];
            format_time(st.st_mtime, time_buf);
            print_perms(st.st_mode, st.st_type);
            printf("%d\t%s\t%s\n", st.st_size, time_buf, entries[i].d_name);
        } else {
            printf("??????????\t???? ?? ??:??\t%s\n", entries[i].d_name);
        }
    }

    if (count == 0 && argc > 1) {
        struct_stat_t st;
        if (sys_stat(path, &st) == 0) {
            char time_buf[20];
            format_time(st.st_mtime, time_buf);
            print_perms(st.st_mode, st.st_type);
            printf("%d\t%s\t%s\n", st.st_size, time_buf, path);
        }
    }

    return 0;
}

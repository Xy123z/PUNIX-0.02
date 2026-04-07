#include <punix.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/wait.h>
#include <stdlib.h>
#define MAX_ARGS 32
#define MAX_PATH 256
#define MAX_CMD_LEN 256
#define MAX_JOBS 32

extern void* signal(int signum, void* handler);

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

typedef struct {
    int id;
    int pgid;
    char cmd[MAX_CMD_LEN];
    job_state_t state;
    termios_t tmodes;
    int tmodes_saved;
} job_t;

job_t jobs[MAX_JOBS];
int next_job_id = 1;
int shutdown_pid = 0;
termios_t shell_tmodes;

void broadcast_message(const char* message) {
    char tty_path[16];
    for (int i = 0; i < 4; i++) {
        sprintf(tty_path, "/dev/tty%d", i);
        int fd = open(tty_path, O_WRONLY);
        if (fd >= 0) {
            write(fd, message, strlen(message));
            close(fd);
        }
    }
}

void add_job(int pgid, const char* cmd, job_state_t state) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            strncpy(jobs[i].cmd, cmd, MAX_CMD_LEN - 1);
            jobs[i].state = state;
            tcgetattr(0, &jobs[i].tmodes);
            jobs[i].tmodes_saved = 1;
            printf("[%d]+ Stopped %s\n", jobs[i].id, jobs[i].cmd);
            return;
        }
    }
}

job_t* find_job(int id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid != 0 && jobs[i].id == id) return &jobs[i];
    }
    return NULL;
}

void remove_job(int pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == pgid) {
            jobs[i].pgid = 0;
            return;
        }
    }
}

void update_jobs() {
    int status;
    int pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].pgid == pid) {
                if (WIFSTOPPED(status)) {
                    if (jobs[i].state == JOB_RUNNING) {
                        jobs[i].state = JOB_STOPPED;
                        tcgetattr(0, &jobs[i].tmodes);
                        jobs[i].tmodes_saved = 1;
                        printf("[%d]+ Stopped %s\n", jobs[i].id, jobs[i].cmd);
                    }
                } else {
                    printf("[%d]+ Done %s\n", jobs[i].id, jobs[i].cmd);
                    jobs[i].pgid = 0;
                }
                break;
            }
        }
    }
}

const char* default_search_paths[] = {
    "/bin/",
    "/sbin/",
    "/usr/bin/",
    "/usr/sbin/",
    NULL
};

void show_prompt() {
    update_jobs();
    char cwd[MAX_PATH];
    const char* user = getenv("USER");
    if (!user) user = "unknown";
    
    getcwd(cwd, MAX_PATH);
    uint32_t uid = getuid();
    
    // Prompt polish: replace /home/user with ~
    char prompt_cwd[MAX_PATH];
    char home_prefix[MAX_PATH];
    const char* home = getenv("HOME");
    if (home) {
        strcpy(home_prefix, home);
    } else {
        sprintf(home_prefix, "/home/%s", user);
    }
    
    if (strncmp(cwd, home_prefix, strlen(home_prefix)) == 0) {
        sprintf(prompt_cwd, "~%s", cwd + strlen(home_prefix));
    } else {
        strcpy(prompt_cwd, cwd);
    }
    
    printf("\033[1;36m%s@punix\033[0m:\033[1;32m%s\033[0m%s ", 
           user, prompt_cwd, (uid == 0) ? "#" : "$");
}

int find_executable(const char* cmd, char* full_path) {
    if (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/')) {
        struct_stat_t st;
        if (sys_stat(cmd, &st) == 0) {
            strcpy(full_path, cmd);
            return 1;
        }
        return 0;
    }

    const char* path_env = getenv("PATH");
    printf("%s\n",path_env);
    if (!path_env) {
        for (int i = 0; default_search_paths[i] != NULL; i++) {
            strcpy(full_path, default_search_paths[i]);
            strcat(full_path, cmd);
            struct_stat_t st;
         if (sys_stat(full_path, &st) == 0) return 1;
        }
        return 0;
    }

    char path_buf[512];
    strcpy(path_buf, path_env);
    char* dir = path_buf;
    char* next;
    while (dir) {
        next = strchr(dir, ':');
        if (next) *next = '\0';
        
        strcpy(full_path, dir);
        if (full_path[strlen(full_path)-1] != '/') strcat(full_path, "/");
        strcat(full_path, cmd);
        
        struct_stat_t st;
        if (sys_stat(full_path, &st) == 0) return 1;
        
        if (next) dir = next + 1;
        else dir = NULL;
    }

    return 0;
}

void execute_pipeline(char* cmd) {
    int is_background = 0;
    char* ampersand = strrchr(cmd, '&');
    if (ampersand) {
        *ampersand = '\0';
        is_background = 1;
        // Trim trailing spaces
        char* end = ampersand - 1;
        while (end >= cmd && *end == ' ') { *end = '\0'; end--; }
    }

    char* commands[8];
    int num_cmds = 0;
    
    char* token = cmd;
    char* next_cmd = cmd;
    while ((next_cmd = strchr(token, '|')) != NULL) {
        *next_cmd = '\0';
        commands[num_cmds++] = token;
        token = next_cmd + 1;
    }
    commands[num_cmds++] = token;

    int pids[8];
    int prev_pipe_read = -1;

    int pipeline_pgid = 0;
    for (int i = 0; i < num_cmds; i++) {
        int pipefd[2];
        if (i < num_cmds - 1) pipe(pipefd);

        int pid = fork();
        if (pid == 0) {
            // Child: set PGID
            if (i == 0) pipeline_pgid = getpid();
            setpgid(0, pipeline_pgid);
            
            // Set as foreground PGID of the TTY ONLY if not background
            if (i == 0 && !is_background) {
                signal(22, (void*)1); // SIGTTOU
                ioctl(0, TIOCSPGRP, &pipeline_pgid);
                signal(22, (void*)0); // SIG_DFL
            }

            if (prev_pipe_read != -1) {
                dup2(prev_pipe_read, 0);
                close(prev_pipe_read);
            }
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], 1);
                close(pipefd[1]);
            }

            char* c = commands[i];
            char* out_file = strchr(c, '>');
            char* in_file = strchr(c, '<');
            int append = 0;

            if (out_file) {
                if (*(out_file + 1) == '>') {
                    append = 1;
                    *out_file = '\0';
                    out_file += 2;
                } else {
                    *out_file = '\0';
                    out_file += 1;
                }
                while (*out_file == ' ') out_file++;
                char* end = out_file;
                while (*end && *end != ' ' && *end != '<') end++;
                char saved = *end; *end = '\0';
                
                int fd = open(out_file, append ? O_WRONLY | 0x08 : O_WRONLY | O_CREAT);
                if (fd >= 0) {
                    dup2(fd, 1);
                    close(fd);
                } else {
                    printf("pbash: %s: Cannot open for writing\n", out_file);
                    exit(1);
                }
                *end = saved;
            }

            if (in_file) {
                *in_file = '\0';
                in_file += 1;
                while (*in_file == ' ') in_file++;
                char* end = in_file;
                while (*end && *end != ' ' && *end != '>') end++;
                char saved = *end; *end = '\0';

                int fd = open(in_file, O_RDONLY);
                if (fd >= 0) {
                    dup2(fd, 0);
                    close(fd);
                } else {
                    printf("bash: %s: No such file\n", in_file);
                    exit(1);
                }
                *end = saved;
            }

            char* argv[MAX_ARGS];
            int argc = 0;
            char* arg_token = commands[i];
            while (*arg_token) {
                while (*arg_token == ' ') arg_token++;
                if (*arg_token == '\0') break;
                argv[argc++] = arg_token;
                while (*arg_token && *arg_token != ' ') arg_token++;
                if (*arg_token == ' ') {
                    *arg_token = '\0';
                    arg_token++;
                }
            }
            argv[argc] = NULL;

            if (argc > 0) {
                char path[MAX_PATH];
                if (find_executable(argv[0], path)) {
                    execve(path, argv, environ);
                } else {
                    printf("pbash: %s: command not found\n", argv[0]);
                }
            }
            exit(0);
        } else {
            if (i == 0) pipeline_pgid = pid;
            setpgid(pid, pipeline_pgid); 
            pids[i] = pid;
            if (prev_pipe_read != -1) close(prev_pipe_read);
            if (i < num_cmds - 1) { close(pipefd[1]); prev_pipe_read = pipefd[0]; }
        }
    }

    // Set foreground group ONLY if not background
    if (pipeline_pgid > 0 && !is_background) {
        signal(22, (void*)1); // SIGTTOU
        ioctl(0, TIOCSPGRP, &pipeline_pgid);
        signal(22, (void*)0); // SIG_DFL

        // Wait for foreground job
        for (int i = 0; i < num_cmds; i++) {
            int status;
            int wait_ret = waitpid(pids[i], &status, WUNTRACED);
            printf("waiting for pid %d to return\n",wait_ret);
            if (wait_ret > 0 && WIFSTOPPED(status)) {
                // Suspended by Ctrl+Z or Ctrl+J
                add_job(pipeline_pgid, cmd, JOB_STOPPED);
                if (((status >> 8) & 0xFF) == 23) { // SIGJOB
                    void show_job_menu();
                    show_job_menu();
                }
                break; 
            }
        }

        // Restore shell as foreground group
        signal(22, (void*)1); // SIGTTOU
        uint32_t shell_pgid = getpid();
        ioctl(0, TIOCSPGRP, &shell_pgid);
        tcsetattr(0, TCSADRAIN, &shell_tmodes);
        signal(22, (void*)0); // SIG_DFL
    } else if (pipeline_pgid > 0 && is_background) {
        add_job(pipeline_pgid, cmd, JOB_RUNNING);
        printf("[%d]+ %s &\n", pipeline_pgid, cmd);
    }
}

void read_password(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = getchar();
        if (c == '\n') break;
        if (c == '\b') {
            if (i > 0) i--;
            continue;
        }
        buf[i++] = c;
        // Don't echo password
    }
    buf[i] = '\0';
    printf("\n");
}

void show_job_menu() {
    int daemon_pid = fork();
    if (daemon_pid == 0) {
        // I am the daemon process for rendering
        termios_t t, old_t;
        tcgetattr(0, &old_t);
        t = old_t;
        t.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &t);
        
        int selected = 0;
        int active_jobs[MAX_JOBS];
        int num_jobs = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].pgid != 0 && jobs[i].state == JOB_STOPPED) {
                active_jobs[num_jobs++] = i;
            }
        }
        
        printf("\033[s"); // save cursor
        
        while (1) {
            printf("\033[1;1H"); // move to top left
            printf("\033[44;37m --- JOB MENU DAEMON --- \033[K\n");
            for (int i = 0; i < num_jobs; i++) {
                if (i == selected) printf("\033[46;37m"); // Cyan highlight
                else printf("\033[44;37m");
                
                int j = active_jobs[i];
                printf(" [%d] %s \033[K\n", jobs[j].id, jobs[j].cmd);
            }
            if (num_jobs == 0) {
                printf("\033[44;37m No stopped jobs. \033[K\n");
            }
            for (int i = (num_jobs > 0 ? num_jobs : 1); i < 5; i++) {
                printf("\033[44;37m\033[K\n"); 
            }
            printf("\033[0m"); // reset color
            
            char c = getchar();
            if (c == 27) { // ANSI escape
                c = getchar();
                if (c == '[') {
                    c = getchar();
                    if (c == 'A' && selected > 0) selected--; // Up
                    if (c == 'B' && selected < num_jobs - 1) selected++; // Down
                }
            } else if (c == '\n' || c == '\r') {
                if (num_jobs > 0) {
                    tcsetattr(0, TCSANOW, &old_t);
                    printf("\033[1;1H");
                    for (int i = 0; i < 6; i++) printf("\033[K\n");
                    printf("\033[u");
                    exit(jobs[active_jobs[selected]].id);
                }
            } else if (c == 'q') {
                break;
            }
        }
        
        tcsetattr(0, TCSANOW, &old_t);
        printf("\033[1;1H");
        for (int i = 0; i < 6; i++) printf("\033[K\n");
        printf("\033[u"); // restore cursor
        exit(0);
    }
    
    int status;
    waitpid(daemon_pid, &status, 0);
    
    int selected_id = (status >> 8) & 0xFF;
    if (selected_id > 0) {
        job_t* job = find_job(selected_id);
        if (job) {
            printf("%s\n", job->cmd);
            int pgid = job->pgid;
            job->state = JOB_RUNNING;
            signal(22, (void*)1); // SIGTTOU
            ioctl(0, TIOCSPGRP, &pgid);
            if (job->tmodes_saved) tcsetattr(0, TCSADRAIN, &job->tmodes);
            signal(22, (void*)0); // SIG_DFL

            sys_kill((-pgid) << 8 | 18); // SIGCONT
            
            int wr = waitpid(pgid, &status, WUNTRACED);
            if (wr > 0 && WIFSTOPPED(status)) {
                job->state = JOB_STOPPED;
                tcgetattr(0, &job->tmodes);
                job->tmodes_saved = 1;
                if (((status >> 8) & 0xFF) == 23) {
                    show_job_menu();
                }
            } else {
                remove_job(pgid);
            }
            signal(22, (void*)1); // SIGTTOU
            int shell_pgid = getpid();
            ioctl(0, TIOCSPGRP, &shell_pgid);
            tcsetattr(0, TCSADRAIN, &shell_tmodes);
            signal(22, (void*)0); // SIG_DFL
        }
    }
}

int main() {
    signal(23, (void*)1); // Ignore SIGJOB so pbash intercepts it gracefully

    // Become process group leader
    uint32_t my_pid = getpid();
    setpgid(0, 0);
    ioctl(0, TIOCSPGRP, &my_pid);

    tcgetattr(0, &shell_tmodes); // Save shell's desired terminal state

    char cmd[MAX_CMD_LEN];
    printf("\033[1;32mPUNIX Bash v1.1\033[0m\n");
    printf("Type 'help' for info.\n\n");

    while (1) {
        tcsetattr(0, TCSADRAIN, &shell_tmodes);
        show_prompt();
        
        int i = 0;
        while (i < MAX_CMD_LEN - 1) {
            char c = getchar();
            if ((int)(signed char)c == -1 || c == (char)255) {
                // Input interrupted by SIGJOB!
                void show_job_menu();
                show_job_menu();
                printf("\n");
                show_prompt();
                for (int k = 0; k < i; k++) putchar(cmd[k]);
                continue;
            }
            if (c == '\n') break;
            if (c == '\b') {
                if (i > 0) i--;
                continue;
            }
            if (c >= ' ' && c <= '~') {
                cmd[i++] = c;
            }
        }
        cmd[i] = '\0';

        if (strlen(cmd) == 0) continue;

        char* cptr = cmd;
        while (*cptr == ' ') cptr++;
        if (*cptr == '\0') continue;

        // Built-ins
        if (strncmp(cptr, "exit", 4) == 0 && (cptr[4] == ' ' || cptr[4] == '\0')) {
            break;
        } else if (strncmp(cptr, "cd", 2) == 0 && (cptr[2] == ' ' || cptr[2] == '\0')) {
            char* path = cptr + 2;
            while (*path == ' ') path++;
            if (*path == '\0') path = getenv("HOME");
            if (!path) path = "/";
            if (chdir(path) != 0) {
                printf("cd: %s: No such directory\n", path);
            } else {

                char new_cwd[MAX_PATH];
                getcwd(new_cwd, MAX_PATH);
                setenv("PWD", new_cwd, 1);
            }
            continue;
        } else if (strncmp(cptr, "sudo", 4) == 0 && (cptr[4] == ' ' || cptr[4] == '\0')) {
            char* rest = cptr + 4;
            while (*rest == ' ') rest++;
            
            if (*rest == '\0') {
                printf("usage: sudo COMMAND\n");
                continue;
            }

            uint32_t orig_uid = sys_getuid();
            if (orig_uid != 0) {
                char pass[40];
                printf("[sudo] password for root: ");
                read_password(pass, 40);
                if (sys_authenticate(pass) != 0) {
                    printf("sudo: 3 incorrect password attempts\n");
                    continue;
                }
            }
            execute_pipeline(rest);
            if (orig_uid != 0) setuid(orig_uid);
            continue;
        } else if (strcmp(cptr, "help") == 0) {
            printf("PUNIX Bash v1.3\n");
            printf("Built-ins: cd, exit, help, clear, sudo, jobs, fg, bg\n");
            printf("Paths: /bin, /sbin, /usr/bin, /usr/sbin\n");
            printf("Features: pipes (|), redirection (>, >>, <), job control (Ctrl+Z, bg, fg)\n");
            continue;
        } else if (strcmp(cptr, "clear") == 0) {
            printf("\x1b[2J\x1b[H");
            continue;
        } else if (strcmp(cptr, "jobs") == 0) {
            update_jobs();
            for (int i = 0; i < MAX_JOBS; i++) {
                if (jobs[i].pgid != 0) {
                    printf("[%d]+ %s  %s\n", jobs[i].id, 
                           (jobs[i].state == JOB_STOPPED) ? "Stopped" : 
                           (jobs[i].state == JOB_RUNNING) ? "Running" : "Done",
                           jobs[i].cmd);
                }
            }
            continue;
        } else if (strncmp(cptr, "fg", 2) == 0 && (cptr[2] == ' ' || cptr[2] == '\0')) {
            char* arg = cptr + 2;
            while (*arg == ' ') arg++;
            int jid = 1;
            if (*arg == '%') jid = atoi(arg + 1);
            else if (*arg != '\0') jid = atoi(arg);

            job_t* job = find_job(jid);
            if (job) {
                printf("%s\n", job->cmd);
                int pgid = job->pgid;
                job->state = JOB_RUNNING;
                signal(22, (void*)1); // SIGTTOU
                ioctl(0, TIOCSPGRP, &pgid);
                if (job->tmodes_saved) tcsetattr(0, TCSADRAIN, &job->tmodes);
                signal(22, (void*)0); // SIG_DFL

                // SIGCONT = 18. Send to group (-pgid)
                sys_kill((-pgid) << 8 | 18); 
                
                int status;
                int wr = waitpid(pgid, &status, WUNTRACED);
                if (wr > 0 && WIFSTOPPED(status)) {
                    job->state = JOB_STOPPED;
                    tcgetattr(0, &job->tmodes);
                    job->tmodes_saved = 1;
                } else {
                    remove_job(pgid);
                }
                signal(22, (void*)1); // SIGTTOU
                int shell_pgid = getpid();
                ioctl(0, TIOCSPGRP, &shell_pgid);
                tcsetattr(0, TCSADRAIN, &shell_tmodes);
                signal(22, (void*)0); // SIG_DFL
            } else {
                printf("bash: fg: %s: no such job\n", arg);
            }
            continue;
        } else if (strncmp(cptr, "bg", 2) == 0 && (cptr[2] == ' ' || cptr[2] == '\0')) {
            char* arg = cptr + 2;
            while (*arg == ' ') arg++;
            int jid = 1;
            if (*arg == '%') jid = atoi(arg + 1);
            else if (*arg != '\0') jid = atoi(arg);

            job_t* job = find_job(jid);
            if (job) {
                int pgid = job->pgid;
                job->state = JOB_RUNNING;

                // CRITICAL: Restore the shell as the terminal foreground process group
                // BEFORE sending SIGCONT.  If we do it after, there is a window where
                // the resumed job could read/write the terminal without triggering
                // SIGTTIN/SIGTTOU.  Setting foreground_pgid to the shell's PGID first
                // ensures the job is immediately a background process when it wakes.
                signal(22, (void*)1); // SIGTTOU
                uint32_t shell_pgid = getpid();
                ioctl(0, TIOCSPGRP, &shell_pgid);
                signal(22, (void*)0); // SIG_DFL

                // Now send SIGCONT to resume the stopped job in the background.
                sys_kill((-pgid) << 8 | 18); // SIGCONT=18, negative pid = pgrp

                printf("[%d]+ %s &\n", job->id, job->cmd);
            } else {
                printf("bash: bg: %s: no such job\n", arg);
            }
            continue;
        } else if (strncmp(cptr, "shutdown", 8) == 0 && (cptr[8] == ' ' || cptr[8] == '\0')) {
            char* arg = cptr + 8;
            while (*arg == ' ') arg++;

            if (strcmp(arg, "now") == 0) {
                broadcast_message("\r\nBroadcast message from root@punix: system is going to shutdown NOW\r\n");
                sync();
                sys_shutdown();
            } else if (arg[0] == '+') {
                int seconds = atoi(arg + 1);
                char msg[128];
                sprintf(msg, "\r\nBroadcast message from root@punix: system is going to shutdown in %d seconds\r\n", seconds);
                broadcast_message(msg);
                
                int pid = fork();
                if (pid == 0) {
                    // Child process: sleep then shutdown
                    sleep(seconds);
                    sync();
                    sys_shutdown();
                    exit(0);
                } else {
                    shutdown_pid = pid;
                    printf("Shutdown scheduled for %d seconds from now (PID %d)\n", seconds, pid);
                }
            } else if (strcmp(arg, "-c") == 0) {
                if (shutdown_pid > 0) {
                    sys_kill(shutdown_pid);
                    shutdown_pid = 0;
                    broadcast_message("\r\nBroadcast message from root@punix: shutdown aborted\r\n");
                    printf("Shutdown cancelled.\n");
                } else {
                    printf("shutdown: no scheduled shutdown to cancel\n");
                }
            } else {
                printf("usage: shutdown [now | +seconds | -c]\n");
            }
            continue;
        } else if (strcmp(cptr, "reboot") == 0) {
            broadcast_message("\r\nBroadcast message from root@punix: system is going to reboot NOW\r\n");
            sync();
            sys_restart();
            continue;
        }

        execute_pipeline(cptr);
    }

    return 0;
}

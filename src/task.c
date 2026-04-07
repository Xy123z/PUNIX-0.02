// src/task.c - Process Management (Unix-style)
// Process groups, sessions, signals, zombie reaping.
// TTY allocation is removed — TTY devices live in tty.c.

#include "../include/task.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/gdt.h"
#include "../include/paging.h"
#include "../include/tty.h"
#include "../include/interrupt.h"
#include "../include/fs.h"
#include "../include/loader.h"
#include "../include/syscall.h"
#include "../include/pipe.h"
#include "../include/console.h"
extern task_t*  current_task   = 0;
 task_t* task_list_head = 0;
uint32_t next_pid       = 1;

extern void switch_to(uint32_t* old_esp, uint32_t new_esp);
extern void task_return(void);

static void int_to_str_local(int v, char* buf) {
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return; }
    char tmp[12]; int i=0;
    int neg = (v < 0); if (neg) v = -v;
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    if (neg) tmp[i++] = '-';
    for (int j=0; j<i; j++) buf[j] = tmp[i-1-j];
    buf[i] = '\0';
}

static void task_default_signal_action(int sig);

// ─── task_init ───────────────────────────────────────────────────────────
void task_init(void) {
    current_task = (task_t*)kmalloc(sizeof(task_t));
    if (!current_task) return;
    memset(current_task, 0, sizeof(task_t));

    current_task->id         = 0;  // PID 0 = kernel swapper
    current_task->parent_id  = 0;
    current_task->uid        = 0;
    current_task->gid        = 0;
    current_task->pgid       = 0;
    current_task->sid        = 0;
    current_task->ctrl_tty   = -1;
    current_task->state      = TASK_RUNNING;
    strcpy(current_task->name, "swapper");
    current_task->page_directory = current_page_directory;
    current_task->kernel_stack   = 0x90000;
    current_task->kernel_esp     = 0;
    current_task->heap_end       = 0x81000000;
    current_task->cwd_id         = 1;
    strcpy(current_task->username, "root");

    // V7-style priority: start at PUSER, no CPU usage, no nice
    current_task->p_pri  = PUSER;
    current_task->p_cpu  = 0;
    current_task->p_nice = 0;

    // Stdin/stdout/stderr → tty0
    tty_device_t* tty0 = tty_get(0);
    if (tty0) {
        current_task->ctrl_tty = 0;
        current_task->fd_table[0].in_use = 1;
        current_task->fd_table[0].type   = FD_TYPE_TTY;
        current_task->fd_table[0].ptr    = tty0;
        current_task->fd_table[0].flags  = O_RDONLY;

        current_task->fd_table[1].in_use = 1;
        current_task->fd_table[1].type   = FD_TYPE_TTY;
        current_task->fd_table[1].ptr    = tty0;
        current_task->fd_table[1].flags  = O_WRONLY;

        current_task->fd_table[2].in_use = 1;
        current_task->fd_table[2].type   = FD_TYPE_TTY;
        current_task->fd_table[2].ptr    = tty0;
        current_task->fd_table[2].flags  = O_WRONLY;
    }

    task_list_head = current_task;
    tss_set_stack(current_task->kernel_stack);
}

// ─── task_create ─────────────────────────────────────────────────────────
// tty_index: which tty_device_t to use for stdin/stdout/stderr (-1 = inherit from parent)
task_t* task_create(uint32_t parent_id, page_directory_t* dir, int tty_index) {
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) return 0;
    memset(task, 0, sizeof(task_t));

    task->id        = next_pid++;
    task->parent_id = parent_id;
    task->uid       = current_task ? current_task->uid : 1000;
    task->gid       = current_task ? current_task->gid : 1000;
    task->pgid      = current_task ? current_task->pgid : task->id;
    task->sid       = current_task ? current_task->sid  : task->id;
    task->ctrl_tty  = -1;
    task->state     = TASK_NEW;
    strcpy(task->name, "new_process");
    task->page_directory = dir;
    task->heap_end = 0x81000000;
    task->cwd_id = current_task ? current_task->cwd_id : 1;

    if (current_task) {
        strcpy(task->username, current_task->username);
    } else {
        strcpy(task->username, "root");
    }
    task->environ = NULL;

    // V7-style priority: new processes start at PUSER
    task->p_pri  = PUSER;
    task->p_cpu  = 0;
    task->p_nice = 0;

    // Determine which TTY to attach
    int ti = tty_index;
    if (ti < 0 && current_task) ti = current_task->ctrl_tty;

    tty_device_t* tty = (ti >= 0) ? tty_get(ti) : 0;
    if (tty) {
        task->ctrl_tty = ti;
        task->fd_table[0].in_use = 1;
        task->fd_table[0].type   = FD_TYPE_TTY;
        task->fd_table[0].ptr    = tty;
        task->fd_table[0].flags  = O_RDONLY;

        task->fd_table[1].in_use = 1;
        task->fd_table[1].type   = FD_TYPE_TTY;
        task->fd_table[1].ptr    = tty;
        task->fd_table[1].flags  = O_WRONLY;

        task->fd_table[2].in_use = 1;
        task->fd_table[2].type   = FD_TYPE_TTY;
        task->fd_table[2].ptr    = tty;
        task->fd_table[2].flags  = O_WRONLY;
    }

    // Allocate kernel stack
    task->status_reported = 0;
    task->kernel_stack = (uint32_t)pmm_alloc_page();
    if (!task->kernel_stack) { kfree(task); return 0; }
    task->kernel_stack += PAGE_SIZE;
    task->kernel_esp    = task->kernel_stack;

    // Prepend to task list
    task->next       = task_list_head;
    task_list_head   = task;
    return task;
}

// ─── task_fork ───────────────────────────────────────────────────────────
task_t* task_fork(registers_t* regs) {
    page_directory_t* new_dir = paging_clone_directory(current_task->page_directory);
    if (!new_dir) return 0;

    task_t* child = task_create(current_task->id, new_dir, current_task->ctrl_tty);
    if (!child) { paging_free_directory(new_dir); return 0; }

    child->user_stack_top = current_task->user_stack_top;
    child->heap_end = current_task->heap_end;
    child->uid  = current_task->uid;
    child->gid  = current_task->gid;
    child->pgid = current_task->pgid;
    child->sid  = current_task->sid;
    strcpy(child->name, current_task->name);
    strcpy(child->username, current_task->username);
    child->state = TASK_READY;

    // Copy FD table
    for (int i = 0; i < MAX_FDS; i++) {
        child->fd_table[i] = current_task->fd_table[i];
        if (child->fd_table[i].in_use && child->fd_table[i].type == FD_TYPE_PIPE) {
            pipe_t* p = (pipe_t*)child->fd_table[i].ptr;
            if ((child->fd_table[i].flags & 3) == O_RDONLY) p->readers++;
            else if ((child->fd_table[i].flags & 3) == O_WRONLY) p->writers++;
        }
    }

    // Copy signal handlers
    for (int i = 0; i < NSIG; i++)
        child->signal_handlers[i] = current_task->signal_handlers[i];

    // Copy environment
    if (current_task->environ) {
        int env_count = 0;
        while (current_task->environ[env_count]) env_count++;
        child->environ = (char**)kmalloc((env_count + 1) * sizeof(char*));
        for (int i = 0; i < env_count; i++) {
            child->environ[i] = (char*)kmalloc(strlen(current_task->environ[i]) + 1);
            strcpy(child->environ[i], current_task->environ[i]);
        }
        child->environ[env_count] = NULL;
    } else {
        child->environ = NULL;
    }

    // Set up kernel stack so child returns from the syscall with eax=0
    uint32_t* stack = (uint32_t*)child->kernel_stack;
    *(--stack) = regs->ss;
    *(--stack) = regs->esp;
    *(--stack) = regs->eflags;
    *(--stack) = regs->cs;
    *(--stack) = regs->eip;
    *(--stack) = 0;         // error code
    *(--stack) = 0x80;      // interrupt number
    *(--stack) = 0;         // eax = 0 (child)
    *(--stack) = regs->ecx;
    *(--stack) = regs->edx;
    *(--stack) = regs->ebx;
    *(--stack) = regs->esp;
    *(--stack) = regs->ebp;
    *(--stack) = regs->esi;
    *(--stack) = regs->edi;
    *(--stack) = regs->ds;
    *(--stack) = regs->es;
    *(--stack) = regs->fs;
    *(--stack) = regs->gs;
    *(--stack) = (uint32_t)task_return;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    child->kernel_esp = (uint32_t)stack;

    return child;
}

// ─── task_replace (exec) ─────────────────────────────────────────────────
void task_replace(task_t* task, uint32_t eip, uint32_t esp) {
    uint32_t* stack = (uint32_t*)task->kernel_stack;
    *(--stack) = 0x23;   // ss (user data)
    *(--stack) = esp;
    *(--stack) = 0x202;  // eflags
    *(--stack) = 0x1B;   // cs (user code)
    *(--stack) = eip;
    *(--stack) = 0;      // error code
    *(--stack) = 0x80;   // interrupt number
    *(--stack) = 0;      // eax
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = esp;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0x23;   // ds
    *(--stack) = 0x23;   // es
    *(--stack) = 0x23;   // fs
    *(--stack) = 0x23;   // gs
    *(--stack) = (uint32_t)task_return;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    task->kernel_esp = (uint32_t)stack;
}

// ─── task_find ───────────────────────────────────────────────────────────
task_t* task_find(uint32_t pid) {
    task_t* t = task_list_head;
    while (t) { if (t->id == pid) return t; t = t->next; }
    return 0;
}

// ─── task_do_cleanup ─────────────────────────────────────────────────────
static void task_do_cleanup(task_t* t, int status) {
    if (t->id == 1) {
        // Init died — system halts
        extern int tty_dev_write(tty_device_t*, const char*, int);
        extern void serial_print(const char*);
        const char* msg = "\n[ FATAL ] Init (PID 1) terminated! Halted.\n";
        serial_print(msg);
        if (active_tty)
            tty_dev_write(active_tty, msg, 44);
        while(1) __asm__ volatile("cli; hlt");
    }

    // If this session leader owned a TTY, send SIGHUP to foreground group
    if (t->sid == t->id && t->ctrl_tty >= 0) {
        tty_device_t* tty = tty_get(t->ctrl_tty);
        if (tty && tty->foreground_pgid)
            task_send_signal_pgrp(tty->foreground_pgid, SIGHUP);
    }

    // Close all FDs
    extern void syscall_close_all(task_t*);
    syscall_close_all(t);

    // Free environment
    if (t->environ) {
        for (int i = 0; t->environ[i]; i++) kfree(t->environ[i]);
        kfree(t->environ);
        t->environ = NULL;
    }

    t->exit_status = status;

    // Wake parent waiting for us
    if (t->parent_id) {
        task_t* parent = task_find(t->parent_id);
        if (parent && (parent->wait_pid == t->id || parent->wait_pid == (uint32_t)-1))
            parent->state = TASK_READY;
        // Send SIGCHLD to parent
        task_send_signal(t->parent_id, SIGCHLD);
    }

    // Orphan adoption: reparent children to PID 1 (or PID 0 if init itself died)
    task_t* scan = task_list_head;
    uint32_t new_parent = (t->id == 1) ? 0 : 1;
    while (scan) {
        if (scan->parent_id == t->id) scan->parent_id = new_parent;
        scan = scan->next;
    }

    t->state = TASK_ZOMBIE;
}

// ─── task_exit ───────────────────────────────────────────────────────────
void task_exit(int status) {
    if (!current_task) return;
    task_do_cleanup(current_task, status);
    while (1) { schedule(); __asm__ volatile("hlt"); }
}

// ─── Priority Scheduler Decay (Called every 1 second) ──────────────────
void sched_decay(void) {
    task_t* t = task_list_head;
    while (t) {
        if (t->id != 0) {  // Skip swapper
            // Decay CPU usage: p_cpu = p_cpu / 2
            t->p_cpu >>= 1;

            // Recalculate priority: p_pri = PUSER + (p_cpu / 2) + p_nice
            int new_pri = PUSER + (t->p_cpu >> 1) + t->p_nice;
            
            // Clamp to 0-255
            if (new_pri < 0) new_pri = 0;
            if (new_pri > 255) new_pri = 255;
            t->p_pri = (uint8_t)new_pri;
        }
        t = t->next;
    }
}

// ─── schedule (Unix V7 Priority-based) ──────────────────────────────────
void schedule(void) {
    if (!current_task) return;

    static task_t* last_scheduled = 0;
    task_t* best_task = 0;
    int best_pri = 256;

    // Fair Round-Robin: Start scanning from the task after the last scheduled one.
    // This prevents tasks at the head of the list (like PID 1) from being favored
    // in priority ties.
    task_t* head = (last_scheduled && last_scheduled->next) ? last_scheduled->next : task_list_head;
    task_t* scan = head;
    
    // We do two passes if needed to cover everyone
    for (int pass = 0; pass < 2; pass++) {
        while (scan) {
            if (scan->state == TASK_READY || scan->state == TASK_NEW) {
                if (scan->p_pri < best_pri) {
                    best_pri = scan->p_pri;
                    best_task = scan;
                }
            }
            scan = scan->next;
        }
        if (best_task && best_pri < 256) break; // Found something in first pass
        scan = task_list_head; // Wrap around for second pass
    }

    // If current_task is still RUNNING and nothing better is ready, keep it
    if (!best_task || (current_task->state == TASK_RUNNING && current_task->p_pri <= best_pri)) {
        if (current_task->state == TASK_RUNNING) {
            last_scheduled = current_task;
            return;
        }
        // If current isn't running and we found nothing, pick idle...
        if (!best_task) {
            task_t* t = task_list_head;
            while(t && t->id != 0) t = t->next;
            if (!t || t == current_task) return;
            best_task = t;
        }
    }

    if (best_task != current_task) {
        task_t* old = current_task;

        if (old->state == TASK_RUNNING) old->state = TASK_READY;
        best_task->state = TASK_RUNNING;
        current_task = best_task;
        last_scheduled = best_task;
        paging_switch_directory(best_task->page_directory);
        tss_set_stack(best_task->kernel_stack);
        switch_to(&old->kernel_esp, best_task->kernel_esp);
    }
}

// ─── task_sleep ──────────────────────────────────────────────────────────
void task_sleep(uint32_t ticks) {
    if (!current_task) return;
    current_task->sleep_ticks = ticks;
    current_task->state = TASK_WAITING;
    while (current_task->state == TASK_WAITING) {
        schedule();
        if (current_task->state == TASK_WAITING)
            __asm__ volatile("sti; hlt; cli");
    }
}

void task_update_sleep(void) {
    task_t* t = task_list_head;
    while (t) {
        if (t->state == TASK_WAITING) {
            if (t->sleep_ticks > 0) {
                if (--t->sleep_ticks == 0) t->state = TASK_READY;
            } else {
                // If sleep_ticks is 0, it was likely a sleep(0) yield
                t->state = TASK_READY;
            }
        }
        t = t->next;
    }
}

// ─── task_switch / task_run ──────────────────────────────────────────────
void task_switch(task_t* task) {
    if (!task || task == current_task) return;
    current_task = task;
    paging_switch_directory(task->page_directory);
    tss_set_stack(task->kernel_stack);
}

void task_run(task_t* task) {
    // Update active TTY so keyboard goes to this session's TTY
    // REMOVED: if (task->ctrl_tty >= 0) tty_switch(task->ctrl_tty); 
    // This was causing the VGA hardware to flip when switching to a background task.

    current_task = task;
    task->state  = TASK_RUNNING;
    paging_switch_directory(task->page_directory);
    tss_set_stack(task->kernel_stack);

    __asm__ volatile(
        "mov %0, %%esp\n"
        "pop %%edi\n"
        "pop %%esi\n"
        "pop %%ebx\n"
        "pop %%ebp\n"
        "ret\n"
        : : "r"(task->kernel_esp)
    );
    __builtin_unreachable();
}

// ─── task_get_procs ──────────────────────────────────────────────────────
int task_get_procs(proc_info_t* buf, int max) {
    int count = 0;
    task_t* t = task_list_head;
    while (t && count < max) {
        if (t->state != TASK_TERMINATED && t->id != 0) {
            buf[count].pid   = t->id;
            buf[count].ppid  = t->parent_id;
            buf[count].state = (uint32_t)t->state;
            strcpy(buf[count].name, t->name);
            count++;
        }
        t = t->next;
    }
    return count;
}

// ─── task_kill ───────────────────────────────────────────────────────────
int task_kill_with_sig(uint32_t pid, int sig) {
    if (pid <= 0) return -1;
    task_t* t = task_find(pid);
    if (!t || t->state == TASK_ZOMBIE) return -1;

    if (sig == SIGKILL || sig == SIGTERM || sig == SIGINT || sig == SIGQUIT) {
        if (t == current_task) task_exit(128 + sig);
        task_do_cleanup(t, 128 + sig);
        return 0;
    }

    task_send_signal(pid, sig);
    return 0;
}

// ─── issig: Check if a deliverable signal is pending (Unix V7 equivalent) ──
// Returns 1 if at least one pending, unmasked signal would DO something
// (i.e., has a user handler, or has a default action that is not "ignore").
// Informational signals with a default action of "ignore" (SIGCHLD, SIGCONT)
// return 0 — they do not interrupt interruptible sleeps.
int issig(void) {
    if (!current_task) return 0;
    uint32_t pending = current_task->pending_signals & ~current_task->signal_mask;
    if (!pending) return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1u << sig))) continue;

        void* handler = current_task->signal_handlers[sig];

        // SIG_IGN: explicitly ignored
        if (handler == (void*)1) {
            if (sig == 23) {
                current_task->pending_signals &= ~(1u << sig);
                return 1; // SIGJOB still interrupts blocking I/O to alert userspace
            }
            continue;
        }

        // User-defined handler: always interrupt (will be dispatched by task_deliver_signals)
        if (handler != 0) return 1;

        // Default action: check if meaningful or just informational
        switch (sig) {
            case SIGCHLD: continue;  // Default: ignore
            case SIGCONT: continue;  // Default: continue (no-op at this point)
            default:      return 1;  // Default: terminate, stop, or core — all interrupt
        }
    }
    return 0;
}

// ─── task_deliver_signals ("psig"): deliver signals at end of every syscall ─
// This is the Unix V7 "psig" equivalent. Called just before returning to user
// space. Dispatches each pending, deliverable signal in order.
void task_deliver_signals(void) {
    if (!current_task) return;
    uint32_t pending = current_task->pending_signals & ~current_task->signal_mask;
    if (!pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1u << sig))) continue;

        // Atomically consume the signal
        current_task->pending_signals &= ~(1u << sig);

        void* handler = current_task->signal_handlers[sig];

        if (handler == (void*)1) {
            // SIG_IGN: explicitly ignored, nothing to do
            continue;
        } else if (handler != 0) {
            // User-defined handler — TODO: set up signal trampoline on user stack.
            // For now fall through to default (sufficient for essential job control).
            task_default_signal_action(sig);
        } else {
            // Default action
            task_default_signal_action(sig);
        }
    }
}

// ─── task_send_signal ────────────────────────────────────────────────────────
// Posts a signal to a task. Only wakes interruptible sleepers (sleep_interruptible=1).
// Non-interruptible sleepers (TASK_IO, sleep_interruptible=0) receive the pending bit
// but stay sleeping — they will be delivered the signal when their I/O completes.
void task_send_signal(uint32_t pid, int sig) {
    if (sig <= 0 || sig >= NSIG) return;
    task_t* t = task_find(pid);
    if (!t || t->state == TASK_ZOMBIE) return;

    if (sig == SIGCONT) {
        // Clear ALL pending stop signals, including SIGTTIN/SIGTTOU.
        // Without this, a process resumed from a background-read stop would
        // immediately re-stop on the still-pending SIGTTIN/SIGTTOU bit.
        t->pending_signals &= ~((1u << SIGTSTP) | (1u << SIGSTOP)
                              | (1u << SIGTTIN) | (1u << SIGTTOU));
        if (t->state == TASK_STOPPED || t->state == TASK_WAITING) {
            t->state = TASK_READY;
            t->status_reported = 0;
        }
        // SIGCONT is not itself posted as a pending signal unless the task has
        // a user-installed handler — the state transition above is sufficient.
        return;
    }

    // Post the signal
    t->pending_signals |= (1u << sig);

    // Wake the task if it is in an interruptible sleep.
    // TASK_WAITING + sleep_interruptible=1  → wake immediately.
    // TASK_IO      (sleep_interruptible=0)  → do NOT wake; signal pending until I/O done.
    if (t->state == TASK_WAITING && t->sleep_interruptible) {
        t->state = TASK_READY;
    }
}

void task_send_signal_pgrp(uint32_t pgid, int sig) {
    task_t* t = task_list_head;
    while (t) {
        if (t->pgid == pgid) task_send_signal(t->id, sig);
        t = t->next;
    }
}

// Update task_default_signal_action
static void task_default_signal_action(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
        case SIGKILL:
        case SIGQUIT:
            task_exit(128 + sig);
            break;
        case SIGTSTP:
        case SIGSTOP:
        case SIGTTIN:
        case SIGTTOU:
        case 23: // SIGJOB
            if (current_task) {
                current_task->state = TASK_STOPPED;
                current_task->exit_status = (sig << 8) | 0x7F; // Proper stop status formatting
                current_task->status_reported = 0;
                // Wake parent so wait() returns status change
                if (current_task->parent_id) {
                    task_t* p = task_find(current_task->parent_id);
                    if (p && p->state == TASK_WAITING) p->state = TASK_READY;
                }
                schedule();
            }
            break;
        case SIGCHLD:
        case SIGCONT:
        default:
            break; // Ignore
    }
}

int task_wait(uint32_t pid, int* status, int options) {
    if (!current_task) return -1;

    // Unix-style wait options (from wait.h)
    #define WNOHANG    1
    #define WUNTRACED  2

    while (1) {
        task_t* child = 0;
        int has_children = 0;
        task_t* scan = task_list_head;

        while (scan) {
            if (scan->parent_id == current_task->id) {
                if (pid == (uint32_t)-1 || scan->id == pid) {
                    has_children = 1;
                    if (scan->state == TASK_ZOMBIE || (scan->state == TASK_STOPPED && (options & WUNTRACED) && !scan->status_reported)) {
                        child = scan;
                        break;
                    }
                }
            }
            scan = scan->next;
        }

        if (!has_children) return -1;

        if (child) {
            uint32_t cid = child->id;
            if (child->state == TASK_STOPPED) {
                // Report the ACTUAL stop signal (SIGTSTP, SIGTTIN, SIGTTOU, etc.)
                // The signal number is stored in exit_status by task_default_signal_action.
                // Format: (signal << 8) | 0x7F  (WIFSTOPPED macro checks the 0x7F byte)
                if (status) *status = child->exit_status;
                child->status_reported = 1;
                // We DON'T remove stopped children from the list
                return (int)cid;
            }

            // Zombie cleanup
            task_t* prev = 0, *cur = task_list_head;
            while (cur && cur != child) { prev = cur; cur = cur->next; }
            if (prev) prev->next = child->next;
            else task_list_head = child->next;

            if (child->page_directory && child->page_directory != kernel_page_directory)
                paging_free_directory(child->page_directory);
            if (child->kernel_stack)
                pmm_free_page((void*)(child->kernel_stack - PAGE_SIZE));
            if (status) *status = child->exit_status << 8; // Standard exit status in high byte
            kfree(child);
            return (int)cid;
        }

        if (options & WNOHANG) return 0;

        current_task->wait_pid = (pid == (uint32_t)-1) ? (uint32_t)-1 : pid;
        current_task->state = TASK_WAITING;
        current_task->p_pri = PWAIT; // Boost to kernel priority while waiting
        schedule();
        if (current_task->state == TASK_WAITING)
            __asm__ volatile("sti; hlt; cli");
    }
}

// ─── Session / Process group ────────────────────────────────────────────
int task_setsid(void) {
    if (!current_task) return -1;
    // Cannot create a new session if already a process group leader
    task_t* scan = task_list_head;
    while (scan) {
        if (scan != current_task && scan->pgid == current_task->id) return -1;
        scan = scan->next;
    }
    current_task->sid      = current_task->id;
    current_task->pgid     = current_task->id;
    current_task->ctrl_tty = -1;  // Detach from controlling TTY
    return (int)current_task->sid;
}

int task_setpgid(uint32_t pid, uint32_t pgid) {
    task_t* t = (pid == 0) ? current_task : task_find(pid);
    if (!t) return -1;
    t->pgid = (pgid == 0) ? t->id : pgid;
    return 0;
}

// ─── TTY switch helper (keyboard hotkey) ─────────────────────────────────
void task_tty_switch(int n) {
    tty_switch(n);
}

// src/pipe.c — Unix V7 Aligned Pipe Implementation
// Blocking uses interruptible / non-interruptible sleep, matched to Unix V7.
// issig() is used for signal checks — no ad-hoc bitmask hacks.
#include "../include/pipe.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/task.h"

// ─── Forward declaration of task list head (defined in task.c) ───────────
extern task_t* task_list_head;

// ─── pipe_block_interruptible ─────────────────────────────────────────────
// Blocks the current task in an interruptible sleep (TASK_WAITING).
// Signals with a deliverable action (issig() > 0) will wake the task and
// cause pipe_block_interruptible() to return 1 (EINTR).
// Returns: 0 = woken by data / normal, 1 = woken by signal (EINTR)
static int pipe_block_interruptible(void) {
    current_task->sleep_interruptible = 1;
    current_task->state = TASK_WAITING;
    current_task->p_pri = PIPE; // Kernel priority for pipe operations
    __asm__ volatile("sti");
    schedule();
    __asm__ volatile("cli");
    current_task->sleep_interruptible = 0;

    // After wakeup: check if a deliverable signal is now pending
    if (issig()) return 1; // EINTR
    return 0;
}

// ─── pipe_block_uninterruptible ───────────────────────────────────────────
// Blocks the current task in a non-interruptible sleep (TASK_IO).
// Signals do NOT wake this task — they are delivered after I/O completes.
// Used by the TTY line discipline polling the keyboard input pipe.
static void pipe_block_uninterruptible(void) {
    current_task->sleep_interruptible = 0;
    current_task->state = TASK_IO;
    current_task->p_pri = PIPE; // Kernel priority for pipe operations
    __asm__ volatile("sti");
    schedule();
    __asm__ volatile("cli");
}

// ─── Wake all tasks blocked on a given pipe FD direction ──────────────────
// dir: O_RDONLY wakes readers, O_WRONLY wakes writers
static void pipe_wake(pipe_t* pipe, uint8_t dir) {
    task_t* t = task_list_head;
    while (t) {
        if (t->state == TASK_WAITING || t->state == TASK_IO) {
            for (int i = 0; i < MAX_FDS; i++) {
                if (t->fd_table[i].in_use
                    && t->fd_table[i].type == FD_TYPE_PIPE
                    && t->fd_table[i].ptr  == pipe
                    && (t->fd_table[i].flags & 3) == dir)
                {
                    t->state = TASK_READY;
                    break;
                }
            }
        }
        t = t->next;
    }
}

// ─── pipe_create ─────────────────────────────────────────────────────────
pipe_t* pipe_create(void) {
    pipe_t* pipe = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!pipe) return 0;
    memset(pipe, 0, sizeof(pipe_t));
    pipe->readers = 1;
    pipe->writers = 1;
    return pipe;
}

// ─── pipe_destroy ────────────────────────────────────────────────────────
void pipe_destroy(pipe_t* pipe) {
    if (pipe) kfree(pipe);
}

// ─── pipe_read ───────────────────────────────────────────────────────────
// Reads from a pipe. Blocks interruptibly — a deliverable signal (checked via
// issig()) will abort the wait and return -1 (EINTR).
int pipe_read(pipe_t* pipe, uint8_t* buf, uint32_t count) {
    if (!pipe || !buf || count == 0) return -1;

    uint32_t read_bytes = 0;

    while (read_bytes < count) {
        if (pipe->size == 0) {
            // No writers left → EOF
            if (pipe->writers == 0) break;

            // Return partial read rather than blocking indefinitely
            if (read_bytes > 0) break;

            // Block interruptibly — signals that would DO something wake us.
            // issig() inside pipe_block_interruptible() filters out SIGCHLD etc.
            if (pipe_block_interruptible()) {
                return -1; // EINTR — syscall will re-deliver signal
            }
            // Re-check after wakeup
            continue;
        }

        buf[read_bytes] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->size--;
        read_bytes++;

        // Wake any writer that was blocked on a full pipe
        if (pipe->size == PIPE_BUF_SIZE - 1)
            pipe_wake(pipe, O_WRONLY);
    }

    return (int)read_bytes;
}

// ─── pipe_read_uninterruptible ────────────────────────────────────────────
// Like pipe_read but uses non-interruptible sleep (TASK_IO).
// Used by the TTY driver to reliably read from the keyboard input pipe.
int pipe_read_uninterruptible(pipe_t* pipe, uint8_t* buf, uint32_t count) {
    if (!pipe || !buf || count == 0) return -1;

    uint32_t read_bytes = 0;

    while (read_bytes < count) {
        if (pipe->size == 0) {
            if (pipe->writers == 0) break;
            if (read_bytes > 0) break;
            pipe_block_uninterruptible();
            continue;
        }

        buf[read_bytes] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->size--;
        read_bytes++;

        if (pipe->size == PIPE_BUF_SIZE - 1)
            pipe_wake(pipe, O_WRONLY);
    }

    return (int)read_bytes;
}

// ─── pipe_write ──────────────────────────────────────────────────────────
// Writes to a pipe. Blocks interruptibly — a deliverable signal wakes us.
int pipe_write(pipe_t* pipe, const uint8_t* buf, uint32_t count) {
    if (!pipe || !buf || count == 0) return -1;

    // Broken pipe — no readers
    if (pipe->readers == 0) return -1;

    uint32_t written_bytes = 0;

    while (written_bytes < count) {
        if (pipe->size == PIPE_BUF_SIZE) {
            // Broken pipe check again after potential sleep
            if (pipe->readers == 0) return -1;

            // Block interruptibly waiting for reader to consume data
            if (pipe_block_interruptible()) {
                return written_bytes > 0 ? (int)written_bytes : -1;
            }
            continue;
        }

        pipe->buffer[pipe->head] = buf[written_bytes];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        pipe->size++;
        written_bytes++;

        // Wake any reader that was blocked on an empty pipe
        if (pipe->size == 1)
            pipe_wake(pipe, O_RDONLY);
    }

    return (int)written_bytes;
}

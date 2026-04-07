#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <punix_def.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif

#define ROWS 25
#define COLS 80
#define EDIT_ROWS 23

// Buffer
#define BUFFER_SIZE 40960
char buffer[BUFFER_SIZE];
int buffer_len = 0;
int cursor_idx = 0; // Current insertion point in buffer

// Viewport
int scroll_row = 0; // Which text row is at top of screen

// File
char filename[64] = "";
int file_dirty = 0;

// Colors
#define COL_TEXT     0x07 // Light Grey on Black
#define COL_BAR      0x1F // White on Blue
#define COL_STATUS   0x70 // Black on Light Grey

// Raw mode structures
struct termios orig_termios;
int in_raw_mode = 0;

// ANSI Helpers
void set_cursor(int x, int y) {
    printf("\033[%d;%dH", y + 1, x + 1);
}

void clear_screen() {
    printf("\033[2J");
}

void set_color(uint8_t color) {
    // Basic mapping for common colors used in text.c
    // 0x07: Light Grey (37)
    // 0x1F: White (37) on Blue (44)
    // 0x70: Black (30) on Light Grey (47)
    
    if (color == COL_TEXT) {
        printf("\033[0;37;40m");
    } else if (color == COL_BAR) {
        printf("\033[0;37;44m");
    } else if (color == COL_STATUS) {
        printf("\033[0;30;47m");
    } else {
        printf("\033[0m");
    }
}

void draw_string_at(int x, int y, const char* str, uint8_t color) {
    set_cursor(x, y);
    set_color(color);
    printf("%s", str);
    set_color(0); // Reset
}

// Raw Mode Management
void exit_raw_mode() {
    if (in_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        in_raw_mode = 0;
    }
}

void enter_raw_mode() {
    if (!in_raw_mode) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        in_raw_mode = 1;
    }
}

// Helper: Calculate X, Y on screen from cursor_idx
void get_cursor_pos(int* v_row, int* v_col) {
    int r = 0;
    int c = 0;
    for (int i = 0; i < cursor_idx; i++) {
        if (buffer[i] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
            if (c >= COLS) {
                r++;
                c = 0;
            }
        }
    }
    *v_row = r;
    *v_col = c;
}

// Draw entire UI
void draw_ui() {
    // 1. Draw Title Bar
    char title[81];
    memset(title, ' ', 80);
    title[80] = '\0';
    
    char name_disp[40];
    if (strlen(filename) > 0) strcpy(name_disp, filename);
    else strcpy(name_disp, "[New File]");
    
    if (file_dirty) strcat(name_disp, " *");
    
    int pad = (COLS - strlen(name_disp)) / 2;
    if (pad < 0) pad = 0;
    memcpy(title + pad, name_disp, strlen(name_disp));
    draw_string_at(0, 0, title, COL_BAR);

    // 2. Draw Text Content
    int cr = 0; // Current Row
    int cc = 0; // Current Col
    int screen_y = 1; 
    
    char line_buf[COLS + 1];
    int line_idx = 0;
    
    for (int i = 0; i < buffer_len; i++) {
        char c = buffer[i];
        
        if (c == '\n') {
            if (cr >= scroll_row && screen_y + (cr - scroll_row) < ROWS - 2) {
                int draw_y = screen_y + (cr - scroll_row);
                while (line_idx < COLS) line_buf[line_idx++] = ' ';
                line_buf[COLS] = '\0';
                draw_string_at(0, draw_y, line_buf, COL_TEXT);
            }
            cr++;
            cc = 0;
            line_idx = 0;
        } else {
            if (cr >= scroll_row && screen_y + (cr - scroll_row) < ROWS - 2) {
                 if (line_idx < COLS) {
                     line_buf[line_idx++] = c;
                 }
            }
            cc++;
            if (cc >= COLS) {
                if (cr >= scroll_row && screen_y + (cr - scroll_row) < ROWS - 2) {
                    int draw_y = screen_y + (cr - scroll_row);
                    line_buf[COLS] = '\0';
                    draw_string_at(0, draw_y, line_buf, COL_TEXT);
                }
                cr++;
                cc = 0;
                line_idx = 0;
            }
        }
        if (cr > scroll_row + EDIT_ROWS) break; 
    }
    
    if (cr >= scroll_row && screen_y + (cr - scroll_row) < ROWS - 2) {
         int draw_y = screen_y + (cr - scroll_row);
         while (line_idx < COLS) line_buf[line_idx++] = ' ';
         line_buf[COLS] = '\0';
         draw_string_at(0, draw_y, line_buf, COL_TEXT);
         cr++; 
    }
    
    while (cr - scroll_row < EDIT_ROWS) {
        int draw_y = screen_y + (cr - scroll_row);
        if (draw_y < ROWS - 2) {
            memset(line_buf, ' ', COLS);
            line_buf[COLS] = '\0';
            draw_string_at(0, draw_y, line_buf, COL_TEXT);
        }
        cr++;
    }

    // 3. Status/Help Bar
    char help1[81]; memset(help1, ' ', 80); help1[80] = 0;
    char help2[81]; memset(help2, ' ', 80); help2[80] = 0;
    
    sprintf(help1, "^S Save     ^X Exit     ^T Toggle Raw (%s)", in_raw_mode ? "ON" : "OFF");
    sprintf(help2, "^O WriteOut");

    draw_string_at(0, ROWS-2, help1, COL_STATUS);
    draw_string_at(0, ROWS-1, help2, COL_STATUS);

    // 4. Update Cursor
    int vr, vc;
    get_cursor_pos(&vr, &vc);
    
    if (vr < scroll_row) scroll_row = vr;
    if (vr >= scroll_row + (ROWS - 3)) scroll_row = vr - (ROWS - 3) + 1;
    
    int screen_r = 1 + (vr - scroll_row);
    set_cursor(vc, screen_r);
    fflush(stdout);
}

void insert_char(char c) {
    if (buffer_len >= BUFFER_SIZE - 1) return;
    for (int i = buffer_len; i > cursor_idx; i--) {
        buffer[i] = buffer[i-1];
    }
    buffer[cursor_idx] = c;
    buffer_len++;
    cursor_idx++;
    file_dirty = 1;
}

void backspace() {
    if (cursor_idx <= 0) return;
    for (int i = cursor_idx - 1; i < buffer_len - 1; i++) {
        buffer[i] = buffer[i+1];
    }
    buffer_len--;
    cursor_idx--;
    file_dirty = 1;
}

int input_prompt(const char* prompt, char* dest, int max_len) {
    char pbuf[81];
    memset(pbuf, ' ', 80);
    pbuf[80] = 0;
    sprintf(pbuf, "%s: ", prompt);
    
    int input_idx = 0;
    dest[0] = 0;
    
    // Ensure raw mode for prompt
    int was_raw = in_raw_mode;
    enter_raw_mode();

    while(1) {
        draw_string_at(0, ROWS-2, pbuf, COL_BAR);
        draw_string_at(0, ROWS-1, "Enter: Confirm  ^C: Cancel                    ", COL_STATUS);
        draw_string_at(strlen(prompt)+2, ROWS-2, dest, COL_BAR);
        set_cursor(strlen(prompt)+2 + input_idx, ROWS-2);
        fflush(stdout);
        
        int c = getchar();
        if (c == '\n' || c == '\r') {
            if (!was_raw) exit_raw_mode();
            return 1;
        } else if (c == 3) { // Ctrl+C
            if (!was_raw) exit_raw_mode();
            return 0;
        } else if (c == '\b' || c == 127) {
            if (input_idx > 0) {
                input_idx--;
                dest[input_idx] = 0;
                set_cursor(strlen(prompt)+2 + input_idx, ROWS-2);
                printf(" ");
            }
        } else if (c >= 32 && c <= 126) {
            if (input_idx < max_len - 1) {
                dest[input_idx++] = c;
                dest[input_idx] = 0;
            }
        }
    }
}

void save_file() {
    if (strlen(filename) == 0) {
        if (!input_prompt("Filename to write", filename, 63)) return;
    }
    
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, buffer, buffer_len);
        close(fd);
        file_dirty = 0;
        draw_string_at(0, ROWS-2, "File Saved Successfully!                        ", COL_STATUS);
        fflush(stdout);
        sleep(1);
    } else {
        draw_string_at(0, ROWS-2, "Error: Could not save file!                     ", COL_STATUS);
        fflush(stdout);
        sleep(2);
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        strncpy(filename, argv[1], 63);
        int fd = open(filename, O_RDONLY);
        if (fd >= 0) {
            buffer_len = read(fd, buffer, BUFFER_SIZE);
            if (buffer_len < 0) buffer_len = 0;
            close(fd);
            cursor_idx = 0;
        }
    }

    enter_raw_mode();
    clear_screen();
printf("\033[?7l"); // Disable line wrap
    while(1) {
        draw_ui();
        
        int c = getchar();
        if (c == 24) { // Ctrl+X
            if (file_dirty) {
                char ans[10];
                if (input_prompt("Save modified buffer? (y/n)", ans, 5)) {
                    if (ans[0] == 'y' || ans[0] == 'Y') {
                        save_file();
                        break;
                    } else if (ans[0] == 'n' || ans[0] == 'N') {
                        break;
                    }
                }
            } else {
                break;
            }
        } else if (c == 19) { // Ctrl+S
            save_file();
        } else if (c == 20) { // Ctrl+T (Toggle Raw/Canonical)
            if (in_raw_mode) {
                exit_raw_mode();
                draw_string_at(0, ROWS-2, "Switched to CANONICAL mode. Press ENTER to return.", COL_STATUS);
                fflush(stdout);
                getchar(); // Wait for enter
                enter_raw_mode();
            } else {
                enter_raw_mode();
            }
        } else if (c == 15) { // Ctrl+O (Save As)
             char new_name[64];
             if (input_prompt("Save As", new_name, 63)) {
                 strcpy(filename, new_name);
                 save_file();
             }
        } else if (c == '\b' || c == 127) {
            backspace();
        } else if (c == '\n' || c == '\r') {
            insert_char('\n'); 
        } else if (c >= 32 && c <= 126) {
            insert_char(c);
        }
    }
    
    exit_raw_mode();
    clear_screen();
    return 0;
}

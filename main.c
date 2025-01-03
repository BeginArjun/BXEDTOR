#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB_STOP 8
#define EDITOR_QUIT_TIMES 3

enum editorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

enum editorHighlight{
    HL_NORMAL,
    HL_NUMBER,
    HL_MATCH
};

enum OPERATIONS{
    NO_OP = -1,
    INSERT,
    DELETE,
    SAVE
};

// terminal
void die(const char *s){
    perror(s);
    exit(1);
}

typedef struct erow{
    int size;
    char *chars;
    char *render;
    int rsize;
    unsigned char *hl;
} erow;

struct editorConfig{
    // data
    struct termios orig_termios;
    int screenrows;
    int screencols;
    // cursor position
    int cx,cy;
    int numrows;
    int rowoffset;
    int coloffset;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int rx;
    int last_operation;
    int checkpoint[2]; // save the cursor position [row, col]
};

struct abuf{
    char *b;
    int len;
};

struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void updateOperation(int operation);

void abAppend(struct abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

int editorReadKey(){
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b'){
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '['){
            if(seq[1] > 0 && seq[1] <= 9){
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~'){
                    switch(seq[1]){
                    case '1': case '7':
                        return HOME_KEY;
                    case '4': case '8':
                        return END_KEY;
                    case '3':
                        return DEL_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    }
                }
            }
            else{
                switch (seq[1]){
            case 'A':
                return ARROW_UP;
            case 'B':
                return ARROW_DOWN;
            case 'C':
                return ARROW_RIGHT;
            case 'D':
                return ARROW_LEFT;
            }
            }
        }
        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12)!= 12) return -1;
        return getCursorPosition(rows, cols);
    }else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row){
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    int prev_sep = 1;

    int i = 0;
    while( i < row->rsize){
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)){
            row->hl[i] = HL_NUMBER;
            i++;
            prev_sep = 0;
            continue;
        }

        prev_sep = is_separator(c);
        i++;
    }
}

int editorSyntaxToColor(int hl){
    switch(hl){
        case HL_NUMBER: return 34;
        case HL_MATCH: return 36;
        default: return 37;
    }
}

int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++){
        if(row->chars[j] == '\t') rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxtoCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;

    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t')
            cur_rx += (EDITOR_TAB_STOP - 1) - (cur_rx % EDITOR_TAB_STOP);
        cur_rx++;

        if(cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row){
    int tabs = 0, j;

    for(j = 0; j< row->size; j++){
        if(row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1);
    int idx = 0;

    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while(idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
        } else
        row->render[idx++] = row->chars[j];
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at,char *s, size_t len){
    if(at < 0 || at > E.numrows) return;

    erow *new_row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if(new_row == NULL) return;
    E.row = new_row;
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at){
    if(at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorDelChar(){
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if(E.cx > 0 ){
        editorRowDelChar(row, E.cx -1);
        E.cx--;
    }else{
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
    updateOperation(DELETE);
}

/*** editor operations ***/

void updateOperation(int operation){
   switch(operation){
        case INSERT:
            E.last_operation = INSERT;
            break;
        case DELETE:
            E.last_operation = DELETE;
            break;
        default:
            E.last_operation = NO_OP;
            break;
   }
}

void editorInsertChar(int c){
    if(E.cy == E.numrows){
        editorInsertRow(E.numrows,"", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    updateOperation(INSERT);
    E.cx++;
}

void editorInsertNewline(){
    if(E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    }else{
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

/*** file i/o ***/
char *editorRowsToString(int *buflen){
    int total_len = 0;
    int j;
    for(j = 0; j < E.numrows; j++){
        total_len += E.row[j].size + 1;
    }
    *buflen = total_len;

    char *buf = malloc(total_len);
    char *p = buf;
    for(j = 0; j < E.numrows; j++){
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void openEditor(char *filename){
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    if(linelen != -1){
        while((linelen = getline(&line, &linecap, fp)) != -1){
            while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')){
                linelen--;
            }
            editorInsertRow(E.numrows, line, linelen);
        }
    } 
    free(line);
    fclose(fp);
    E.checkpoint[0] = E.cy;
    E.checkpoint[1] = E.cx;

    E.dirty = 0;
}

void editorSave(){
    updateOperation(SAVE);
    if(E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if(E.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1){
        if(ftruncate(fd, len) != -1){
            if(write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.checkpoint[0] = E.cy;
                E.checkpoint[1] = E.cx;
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
        close(fd);
    }
}

/*** find ***/

void editorFindCallback(char *query, int key){
    static int last_match = -1; // -1 if no match found else row number
    static int direction = 1; // 1 for forward, -1 for backward

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if(saved_hl){
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if(key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    }else if(key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }else if(key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }else{
        last_match = -1;
        direction = 1;
    }

    if(last_match == -1) direction = 1;
    int current = last_match;

    int i;
    for(i = 0; i < E.numrows; i++){
        current += direction;
        if(current == -1) current = E.numrows - 1;
        else if(current == E.numrows) current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if(match){
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxtoCx(row, match - row->render);
            E.rowoffset = E.numrows;
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(){
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloffset = E.coloffset;
    int saved_rowoffset = E.rowoffset;
    
    char *query = editorPrompt(" Search: %s (ESC to cancel/ Arrows to Move/ Enter to Confirm)", editorFindCallback);

    if(query){
        free(query);
    }else{
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloffset = saved_coloffset;
        E.rowoffset = saved_rowoffset;
    }
}

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoffset = 0;
    E.coloffset = 0;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.last_operation = NO_OP;
    E.checkpoint[0] = 0;
    E.checkpoint[1] = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 3;
}


// disable raw mode
void disableRawMode(){
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode(){
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr"); // get the original attributes of the terminal
    atexit(disableRawMode); // disableRawMode will be called when the program exits

    struct termios raw = E.orig_termios; // copy the original attributes to raw

    tcgetattr(STDIN_FILENO, &raw); // get the current attributes of the terminal

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // IXON -> ctrl-s, ctrl-q off

    raw.c_oflag &= ~(OPOST); // OPOST -> output processing off

    raw.c_cflag |= (CS8); // CS8 -> set character size to 8 bits

    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // ECHO -> echo off, ICANON -> canonical mode off, ISIG -> ctrl-c, ctrl-z off, IEXTEN -> ctrl-v off

    raw.c_cc[VMIN] = 0; // min number of bytes needed before read() can return
    raw.c_cc[VTIME] = 1; // max time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr"); // set the new attributes of the terminal
}

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if(c == '\r'){
            if(buflen != 0){
                editorSetStatusMessage("");
                return buf;
            }
        }else if(!iscntrl(c) && c < 128){
            if(buflen == bufsize - 1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }else if(c == '\x1b'){
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }else if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen != 0) buf[--buflen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void moveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch(key){
        case ARROW_UP:
           if(E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows) E.cy++;
            break;
        case ARROW_LEFT:
            if(E.cx > 0) {
                E.cx--;
            } else if(E.cy > 0){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size){
                E.cx++;
            } else if(row && E.cx == row->size){
                E.cy++;
                E.cx = 0;
            }
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--)
                    moveCursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (row) E.cx = row->size;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen){
        E.cx = rowlen;
    }

}

void editorProcessKeyPress(){
    static int quit_times = EDITOR_QUIT_TIMES;

    int c = editorReadKey();
    switch(c){
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('x'):
            if(E.dirty && quit_times > 0){
                editorSetStatusMessage("WARING!!! File has unsaved changes. "
                "Press Ctrl-X %d more times to quit.", quit_times);
                quit_times--;
                return;
            }

            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if(E.cy < E.numrows) E.cx = E.row[E.cy].size;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            if (c == PAGE_UP){
                E.cy = E.rowoffset;
            }
            else if (c == PAGE_DOWN){
                E.cy = E.rowoffset + E.screenrows - 1;
                if (E.cy > E.numrows)
                    E.cy = E.numrows;
            }
            int times = E.screenrows;
            while (times--)
                moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            moveCursor(c);
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) moveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = EDITOR_QUIT_TIMES;
}

void splashScreen(struct abuf *ab){
    char welcome[80];
    int welcomelen = snprintf(welcome, sizeof(welcome),
    "BXEDTOR version --- %s", EDITOR_VERSION);
    if(welcomelen > E.screencols) welcomelen = E.screencols;
    int padding = (E.screencols - welcomelen) / 2;
    if(padding){
        abAppend(ab, "~", 1);
        padding--;
    }
    while(padding--) abAppend(ab, " ", 1);
    abAppend(ab, welcome, welcomelen);
}
// Top Bar will display Version of application and Filename
void editorDrawTopBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m]",4);
    char version[80], filename[21];
    int versionlen = snprintf(version, sizeof(version), "BXEDTOR version --- %s", EDITOR_VERSION);
    int filenamelen = snprintf(filename, sizeof(filename), "%s%.20s", E.dirty?"*":"" ,E.filename ? E.filename : "Untitled");
    if(versionlen > E.screencols) versionlen = E.screencols;
    if(filenamelen > E.screencols) filenamelen = E.screencols;

    int padding = (E.screencols - filenamelen) / 2;
    if (padding < 0) padding = 0;

    abAppend(ab, version, versionlen);
    while(versionlen < padding){
        abAppend(ab, " ", 1);
        versionlen++;
    }
    abAppend(ab, filename, filenamelen);
    while(versionlen + filenamelen < E.screencols){
        abAppend(ab, " ", 1);
        versionlen++;
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4);
    char editor_status[80], rstatus[80];
    int len = snprintf(editor_status, sizeof(editor_status), " %s - %d lines",
    E.last_operation == INSERT ? "(INSERT)" : E.last_operation == DELETE ? "(DELETE)" : E.last_operation == SAVE ? "(SAVE)" : ""
    , E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "Row : %d Col : %d", 
    E.cy + 1, E.cx + 1);
    abAppend(ab, editor_status, len);
    while(len < E.screencols){
        if(E.screencols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }else{
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) msglen = E.screencols;
    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorScroll(){
    E.rx = E.cx;
    if(E.cy < E.numrows){
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    
    if(E.cy < E.rowoffset){
        E.rowoffset = E.cy;
    }
    if(E.cy >= E.rowoffset + E.screenrows){
        E.rowoffset = E.cy - E.screenrows + 1;
    }
    if(E.rx < E.coloffset){
        E.coloffset = E.rx;
    }
    if(E.rx >= E.coloffset + E.screencols){
        E.coloffset = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct  abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoffset;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3){
          splashScreen(ab);
      } else {
          abAppend(ab, "~", 1);
      }
    } else {
        int len = E.row[filerow].size - E.coloffset;
        if (len < 0) len = 0;
        if (len > E.screencols) len = E.screencols;
        char *c = &E.row[filerow].render[E.coloffset];
        unsigned char *hl = &E.row[filerow].hl[E.coloffset];
        int current_color = -1;
        int j;
        for(j = 0; j < len; j++){
            if(hl[j] == HL_NORMAL){
                if(current_color != -1){
                    abAppend(ab, "\x1b[39m", 5);
                    current_color = -1;
                }
                abAppend(ab, &c[j], 1);
            }else{
                int color = editorSyntaxToColor(hl[j]);
                if(color != current_color){
                    current_color = color;
                    char buf[16];
                    int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                    abAppend(ab, buf, clen);
                }
                abAppend(ab, &c[j], 1);
            }
        }
        abAppend(ab, &c[j], 1);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawTopBar(&ab);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    abAppend(&ab, "\x1b[H", 3);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 2,( E.rx - E.coloffset) + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void clearScreen(){
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

void checkDirty(){
    if(E.checkpoint[0] != E.cy || E.checkpoint[1] != E.cx){
        E.dirty = 1;
    }else{
        E.dirty = 0;
    }
}

// init
int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if(argc >= 2) openEditor(argv[1]);

    editorSetStatusMessage("HELP : Ctrl-S = save | Ctrl-X = quit | Ctrl-F = find");

    while(1){
        checkDirty();
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
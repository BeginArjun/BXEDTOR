#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>

#define CTRL_KEY(k) ((k) & 0x1f)

// terminal
void die(const char *s){
    perror(s);
    exit(1);
}

struct editorConfig{
    // data
    struct termios orig_termios;
    int screenrows;
    int screencols;
};

struct editorConfig E;

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if(1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12)!= 12) return -1;
        editorReadKey();
        return -1;
    }else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void initEdior(){
    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
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

char editorReadKey(){
    int nread;
    char c;
    while((nread != (read(STDIN_FILENO, &c, 1))) != 1){
        if(nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
}

void editorProcessKeyPress(){
    char c = editorReadKey();
    switch(c){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}



void editorRefreshScreen(){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    editorDrawRows();
    write(STDOUT_FILENO, "\x1b[H", 3);
}

// init
int main(){
    enableRawMode();
    initEdior();

    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
/*** includes ***/

// These are feature test macros, can be read about here: https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
// Essentially, these feature test macros are used to determine 
// what features our header files include
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
// mirrors what CTRL_KEY does which is it strips all but the first 5 bits of a
// character in a byte e.g.: (a --> ASCII (97, 0b01100001), CTRL+A --> ASCII (1,
// 0b00000001))
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

/*
  While E.cx is an index into the chars field of an erow, the E.rx variable will 
  be an index into the render field. If there are no tabs on the current line, 
  then E.rx will be the same as E.cx. If there are tabs, then E.rx will be greater 
  than E.cx by however many extra spaces those tabs take up when rendered.
*/

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenRows;
  int screenCols;
  int numrows;
  erow *row;
  char *filename;
  char status_msg[50];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
	}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr failed");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcrgetattr failed");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcrsetattr failed");
}

// The characters are casted to ints when returned so as to
// allow for Special Character encodings (such as Arrow Keys --> <esc>[A/B/C/D)
// to be mapped to integer values outside of the ASCII range [0, 127]
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) !=
         1) // waiting for STDIN_FILENO to be populated with 1 byte
  {
    if (nread == -1 && errno != EAGAIN)
      die("read failure");
  }

  // if the first byte is <esc>
  if (c == '\x1b') {
    char seq[3];

    // Then we try and read subsequent bytes to see if it's a special char, like
    // an arrow key, else return <esc>
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  // Giving STDOUT the bytes <esc>[ {Ps} n, where Ps = 6,
  // is us (the host) asking VT100 to return its active position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf)) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' && buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++){
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;

  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;  

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++){
    if (row->chars[j] == '\t'){
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';       
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

/*** file i/o ***/
void editorOpen(char *filename)
{
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  // size_t should be used when we want to return a size in bytes,
  // ssize_t should be used if we want to do the above, and allow for a negative
  // value to be stored indicating an error
  size_t linecap = 0;
  ssize_t linelen;
  //getline returns -1 when it reaches EOF
  while ((linelen = getline(&line, &linecap, fp)) != -1)
  {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int size;
};

#define ABUF_INIT { NULL, 0 }

void abAppend(struct abuf *buf, const char *s, int len) {
  char *newString = (char *)realloc(buf->b, buf->size + len);

  if (newString == NULL)
    return;
  memcpy(&newString[buf->size], s, len);
  buf->b = newString;
  buf->size += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows){
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  
  // E.rowoff refers to what's at the top of the screen
  // E.rowoff starts as 0, when we scroll down all the way past 
  // E.screenRows, we adjust E.rowoff in the 2nd if block.
  // If we scroll back up, we adjust E.rowoff to be our cursor value in 1st if block 
  if (E.cy < E.rowoff){
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenRows){
    E.rowoff = E.cy - E.screenRows + 1; 
  }
  if (E.rx < E.coloff){
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screenCols){
    E.coloff = E.rx - E.screenCols + 1;
  }

}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenRows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows)
    {
      if (E.numrows == 0 && y == E.screenRows / 3) {
        char welcome[80];
        int welcomelen =
            snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s",
                    KILO_VERSION); // snprintf loads AT MOST sizeof(welcome)
        // into welcome, from the data (string) given in as the 3rd argument
        if (welcomelen > E.screenCols)
          welcomelen = E.screenCols;
        int padding = (E.screenCols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screenCols) len = E.screenCols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    

    abAppend(ab, "\x1b[K", 3); // default argument 0 (\x1b[0K) erases part of
                               // the line to the right of the cursor.
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4); // sets inverted coloring
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines", 
              E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);

  if (len > E.screenCols) len = E.screenCols;
  abAppend(ab, status, len);
  while (len < E.screenCols){
    if (E.screenCols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3); // sets back to normal coloring
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3); // clearing message bar with <esc>[K sequence
  int msglen = strlen(E.status_msg);
  if (msglen > E.screenCols) msglen = E.screenCols;
  if (msglen && (time(NULL) - E.statusmsg_time < 5))
    abAppend(ab, E.status_msg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  // 'h' and 'l' (set mode, reset mode) are used to turn on/off various terminal
  // features/modes ?25 is for the cursor. Hence, ?25l is to reset the cursor
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3); // This then sets the cursor to position 1;1

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab); // The message bar will dissappear after a key is pressed 5 seconds after kilo startup

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", // H = adjust the cursor position to %d,%d 
            (E.cy - E.rowoff) + 1,
            (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // Now that we've repositioned the cursor, we set it back and turn it on to
  // the user.
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.size);
  abFree(&ab);
}

/* 
... makes this a variadic function
To use them, we have to declare a va_list variable and pass it and the final argument before the ... into a 
va_start, and we must also have a va_end call as seen below. Between va_start and va_end, we must have a call to
va_arg(). But here, vsnprintf does that for us hence we can use our variadic function
Should improve this comment.
*/
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL); // sets the statusmsg_time to the current time
}


/*** input ***/

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  
  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }

}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows)
          E.cx = E.row[E.cy].size;
        break;

    case PAGE_UP:
    case PAGE_DOWN: 
        { // we create a code block so that were able to declare a
            // variable in our switch statement
            if (c == PAGE_UP){
              E.cy = E.rowoff;
            } else if (c == PAGE_DOWN){
              E.cy = E.rowoff + E.screenRows - 1;
              if (E.cy > E.numrows) E.cy = E.numrows;
            }
            
            int times = E.screenRows;
            while (times--) {
              editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        break;
    case ARROW_DOWN:
    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    }
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.status_msg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenRows, &E.screenCols) == -1)
    die("getWindowSize");
  E.screenRows -= 2;
}

int main(int argc, char* argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2)
  {
    editorOpen(argv[1]);
  }
  
  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

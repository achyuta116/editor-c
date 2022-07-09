/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, // So that they don't conflict with regular keypresses 
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
} erow; // Strands for editor row and stores a line of text as a pointer to
// to the dynamically allocated character data and a length.

struct editorConfig {
    struct termios orig_termios;
    int cx, cy; // Cursor x and y positions
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row; // Pointer to store multiple lines
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback) (char *, int));

/*** terminal ***/

// For error handling
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    // Reposition cursor on exit so that characters aren't left on screen
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) 
	die("tcsetattr");
    // Reset terminal flags to original state and throw away pending input
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) 
	die("tcgetattr");
    atexit(disableRawMode);
    // Read current attributes into raw
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // I stands for input flag and XON comes from Ctrl-S and Ctrl-Q which produce 
    // XOFF and XON to pause and resume transmission
    // Ctrl-S and Ctrl-Q can now be read
    // Ctrl-M is read as 10, terminal translating any carraige returns 13, \r as
    //  newlines 10, \n ICRNL turns off this feature
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // Modify struct
    // ECHO turns off echoing characters into terminal i.e when you enter password
    // ICANON turns off canonical mode we will be reading input byte by
    // byte instead of line by line
    // ISIG turns off Ctrl-C & Ctrl-Z SIGINT and SIGTSTP signals
    // IEXTEN turns off Ctrl-V functionality which you could type Ctrl-V
    // then another sequence to print it literally, fixes Ctrl-O in macOS
    raw.c_cflag |= (CS8);
    // Makes sure all characters are 1 byte in length
    raw.c_cc[VMIN] = 0;
    // Sets minimum number of bytes of input needed before read returns
    // 0 so that read returns as soon as there is any input to be read
    raw.c_cc[VTIME] = 1;
    // VTIME sets maximum time to wait before 
    // read returns in 1/10th of a second if read times
    // out it will return 0
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
	die("tcsetattr");
    // Write new term attributes out
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
	if(nread == -1 && errno != EAGAIN) die("read");
    }

    if(c =='\x1b') {
	char seq[3];

	if(read(STDIN_FILENO, &seq[0], 1) !=1) return '\x1b';
	if(read(STDIN_FILENO, &seq[1], 1) !=1) return '\x1b';
	
	// PUP and PDOWN are [5~ and [6~ hence we needed seq to store 3 bytes
	// Home and End keys have many escape sequences depending on OS
	// [1~ [7~ [H OH for home
	// [4~ [8~ [F OF for end 
	// Delete key returns [3~
	if(seq[0] == '[') {
	    if(seq[1] >= '0' && seq[1] <= '9') {
		if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
		if(seq[2] == '~') {
		    switch (seq[1]) {
			case '1': return HOME_KEY;
			case '3': return DEL_KEY;
			case '4': return END_KEY;
			case '5': return PAGE_UP;
			case '6': return PAGE_DOWN;
			case '7': return HOME_KEY;
			case '8': return END_KEY;
		    }
		}
	    } else {
		switch(seq[1]) {
		    case 'A': return ARROW_UP;
		    case 'B': return ARROW_DOWN;
		    case 'C': return ARROW_RIGHT;
		    case 'D': return ARROW_LEFT;
		    case 'H': return HOME_KEY;
		    case 'F': return END_KEY;
		}
	    }
	} else if(seq[0] == 'O') {
	    switch(seq[1]) {
		case 'H': return HOME_KEY;
		case 'F': return END_KEY;
	    }
	}

	return '\x1b';
    } else {
	return c;
    }
}

int getcursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    // n is used to query terminal for status info
    // 6 is  used to get the cursor position
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return 1;

    // Process the reply
    while (i < sizeof(buf) - 1) {
	if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
	if(buf[i] == 'R') break;
	i++;
    }
    buf[i] = '\0';

    // Verify the reply
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // Insert reply's row and column values into rows and cols
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int*cols) {
    struct winsize ws;
    // All come from ioctl.h
    // On success ioctl() will place number of columns and number of rows
    // On failure this will return -1, values could be 0 so we check for it
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
	// Move cursor to the bottom right corner
	if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
	return getcursorPosition(rows, cols);
    } else {
	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
    }
}
/*** row operations ***/
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++) {
	if(row->chars[j] == '\t')
	    rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
	rx++;
    }
    return rx;
}

int editorRowRxToCx(erow * row, int rx) {
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++) {
	if(row->chars[cx] == '\t') {
	    cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
	}
	cur_rx++;

	if(cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++)
	if(row->chars[j] == '\t') tabs++;

    free(row -> render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->size; j++) {
	if(row->chars[j] == '\t') {
	    row->render[idx++] = ' ';
	    while(idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
	} else {
	    row->render[idx++] = row->chars[j];
	}
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if(at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at +1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if( at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if(E.cy == E.numrows) {
	editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == 0) {
	editorInsertRow(E.cy, "", 0);
    } else {
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

void editorDelChar() {
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if(E.cx > 0) {
	editorRowDelChar(row, E.cx - 1);
	E.cx--;
    } else {
	E.cx = E.row[E.cy - 1].size;
	editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
	editorDelRow(E.cy);
	E.cy--;
    }
}
/*** file i/o ***/
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++)
	totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j = 0; j < E.numrows; j++) {
	memcpy(p, E.row[j].chars, E.row[j].size);
	p += E.row[j].size;
	*p = '\n';
	p++;
    }

    return buf;
}

void editorOpen(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);
    // strdup comes from string.h copy of a given string and allocating req
    // memory and assuming you will free that memory

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1 ) {
	while (linelen > 0 && (line[linelen - 1] == '\n' ||
			       line[linelen - 1] == '\r'))
	    linelen--;
	editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if(E.filename == NULL){
	E.filename = editorPrompt("Save as: %s", NULL);
	if (E.filename == NULL) {
	    editorSetStatusMessage("Save aborted");
	    return;
	}
    }
    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
	if(ftruncate(fd, len) != -1) {
	    if(write(fd, buf, len) == len) {
		close(fd);
		free(buf);
		E.dirty = 0;
		editorSetStatusMessage("%d bytes written to disk", len);
		return;
	    }
	}
	close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %d", strerror(errno));
}
/*** find ***/

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key =='\x1b') {
	last_match = -1;
	direction = 1;
	return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
	direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
	direction = -1;
    } else {
	last_match = -1;
	direction = 1;
    }

    if(last_match == -1) direction = 1;
    int current = last_match;

    int i;
    for(i = 0; i < E.numrows; i++) {
	current += direction;
	if(current == -1) current = E.numrows - 1;
	else if(current == E.numrows) current = 0;

	erow *row = &E.row[current];
	char *match = strstr(row->render, query);
	if(match) {
	    last_match = current;
	    E.cy = current;
	    E.cx = editorRowRxToCx(row, match - row->render);
	    E.rowoff = E.numrows;
	    break;
	}
    }
}


void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
	    editorFindCallback);

    if (query) {
	free(query);
    } else {
	E.cx = saved_cx;
	E.cy = saved_cy;
	E.coloff = saved_coloff;
	E.rowoff = saved_rowoff;
    }
}
	    
/*** append buffer ***/
// If there are many small writes to the screen it'll flicker
// It's better to make a big write using a write append buffer
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if(E.cy < E.numrows) {
	E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if(E.cy < E.rowoff) {
	E.rowoff = E.cy;
    }
    if(E.cy >= E.rowoff + E.screenrows) {
	E.rowoff = E.cy - E.screenrows + 1;
    }
    if(E.rx < E.coloff) {
	E.coloff = E.rx;
    }
    if(E.rx >= E.coloff + E.screencols) {
	E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for(y = 0; y < E.screenrows; y++) {
	// Print welcome message
	// Wrapping row drawing code to check whether we are drawing a row that is part of
	// the text
	// buffer or a row that comes after the end of the text buffer
	// To draw a row that's part of the textbuffer, we write out chars of the erow
	// but taking care to truncare renderedline if it goes past end of
	// screen
	int filerow = y + E.rowoff; // Setting row offset for scrolling
	if (filerow >= E.numrows) {
	    // Welcome message only displays if text buffer is empty
	    if(E.numrows == 0 && y == E.screenrows/3) {
		char welcome[80];
		int welcomelen = snprintf(welcome, sizeof(welcome),
		    "Kilo editor -- version %s", KILO_VERSION);
		if (welcomelen > E.screencols) welcomelen = E.screencols;
		int padding = (E.screencols - welcomelen)/2;
		if (padding) {
		    abAppend(ab, "~", 1);
		    padding--;
		}
		while (padding--) abAppend(ab, " ", 1);
		abAppend(ab, welcome, welcomelen);
	    } else {
		abAppend(ab, "~", 1);
	    }
	} else {
	    int len = E.row[filerow].rsize - E.coloff; // Setting col offset
	    if(len < 0) len = 0;
	    // If length becomes negative due to coloff, length is set
	    // to zero so that nothing is printed on screen
	    if(len > E.screencols) len = E.screencols;
	    abAppend(ab, &E.row[filerow].render[E.coloff], len);
	}


	abAppend(ab, "\x1b[K", 3);
	// Clears one line at a time when in loop it seems more optimal
	// to clear each line as we redraw
	// K clears a line at a time 0,1,2 analogous to J's arguments 0 default
	abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
	    E.filename ? E.filename : "[No Name]", E.numrows,
	    E.dirty? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
	    E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    // m command causes the text printed after it to be printed
    // with various attributes
    // bold - 1, underscore - 4, blink - 5
    // inverted colors - 7, 0 clears all attributes [m goes to normal default
    while (len < E.screencols ) {
	if(E.screencols - len == rlen) {
	    abAppend(ab,rstatus,rlen);
	    break;
	} else {
	    abAppend(ab, " ", 1);
	    len++;
	}
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) msglen = E.screencols;
    // Only append message if time is less than 5 seconds since editor started
    // and after pressing a key
    // since we only refresh screen after single keypress
    if(msglen && time(NULL) - E.statusmsg_time < 5)
	abAppend(ab, E.statusmsg, msglen);
}
// Render UI to the screen after each keypress
void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    // Hide the cursor when repainting
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    // Escape sequence starts with an escape character followed by [
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    // Old H command changed to H command with arguments, specifying
    // position we want cursor to move to
    // Add 1 to E.cy and #.cx to convert from 0-index to 1-index of terminal
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
	    E.rx - E.coloff + 1);
    abAppend(&ab, buf, strlen(buf));
    // Show cursor when done h and l are used to turn on and off various
    // terminal features
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}
/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1) {
	editorSetStatusMessage(prompt, buf);
	editorRefreshScreen();

	int c = editorReadKey();
	if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
	    if (buflen != 0) buf[--buflen] = '\0';
	} else if (c == '\x1b') {
	    editorSetStatusMessage("");
	    if (callback) callback(buf, c);
	    free(buf);
	    return NULL;
	} else if(c == '\r') {
	    if(buflen != 0) {
		editorSetStatusMessage("");
		if (callback) callback(buf, c);
		return buf;
	    }
	} else if (!iscntrl(c) && c < 128) {
	    if(buflen == bufsize - 1) {
		bufsize *= 2;
		buf = realloc(buf, bufsize);
	    }
	    buf[buflen++] = c;
	    buf[buflen] = '\0';
	}
	if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key) {
    // To limit scrolling to the right within a line
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
	case ARROW_LEFT:
	    // So that left press at the beginning of a line
	    // Wraps to end of previous line
	    if (E.cx != 0) {
		E.cx--;
	    } else if(E.cy > 0) {
		E.cy--;
		E.cx = E.row[E.cy].size;
	    }
	    break;
	case ARROW_RIGHT:
	    // To limit scrolling to the right within a line
	    if(row && E.cx < row->size) {
		E.cx++;
	    // For the cursor to go to beginning of next line if it
	    // is at the end of this one
	    } else if (row && E.cx == row->size) {
		E.cy++;
		E.cx = 0;
	    }
	    break;
	case ARROW_UP:
	    if(E.cy != 0) E.cy--;
	    break;
	case ARROW_DOWN:
	    if(E.cy < E.numrows) E.cy++;
	    break;
    }
    // Below code is for setting cursor to end character in a row
    // if the cursor is beyond row length
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen ) {
	E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    switch(c) {
	case '\r':
	    editorInsertNewline();
	    break;
	case CTRL_KEY('q'):
	    if(E.dirty && quit_times > 0) {
		editorSetStatusMessage("WARNING!!! FILE HAS UNSAVED CHANGES. "
			"Press Ctrl-Q %d more times to quit.", quit_times);
		quit_times--;
		return;
	    }
	    write(STDOUT_FILENO, "\x1b[2J", 4);
	    write(STDOUT_FILENO, "\x1b[H", 3);
	    // Reposition cursor on exit so 
	    // that characters aren't left on screen
	    exit(0);
	    break;
	case CTRL_KEY('s'):
	    editorSave();
	    break;
	case ARROW_UP:
	case ARROW_LEFT:
	case ARROW_RIGHT:
	case ARROW_DOWN:
	    editorMoveCursor(c);
	    break;
	case HOME_KEY:
	    E.cx = 0;
	    break;
	case END_KEY:
	    if(E.cy < E.numrows)
		E.cx = E.row[E.cy].size;
	    break;
	case CTRL_KEY('f'):
	    editorFind();
	    break;
	case BACKSPACE:
	case CTRL_KEY('h'):
	case DEL_KEY:
	    if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
	    editorDelChar();
	    break;
	case PAGE_UP:
	case PAGE_DOWN:
	    {
		if(c == PAGE_UP) {
		    E.cy = E.rowoff;
		} else if (c == PAGE_DOWN) {
		    E.cy = E.rowoff + E.screenrows - 1;
		    if(E.cy > E.numrows) E.cy = E.numrows;
		}
		int times = E.screenrows;
		while (times--)
		    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
	    }
	    break;
	// Ctrl-L traditionally used to refresh screen but we're doing that
	// after every keypress so we don't have to do anything else to implement
	// that feature
	// Esc is ignored
	case CTRL_KEY('l'):
	case '\x1b':
	    break;
	default:
	    editorInsertChar(c);
	    break;
    }
    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoff = 0;
    E.coloff = 0;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
	editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    
    // Read 1 byte character from input into c
    while(1) {
	editorRefreshScreen();
	editorProcessKeypress();
    }
    return 0;
}

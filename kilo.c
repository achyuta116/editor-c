/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
enum editorKey {
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
struct editorConfig {
    struct termios orig_termios;
    int cx, cy; // Cursor x and y positions
    int screenrows;
    int screencols;
};
struct editorConfig E;

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
	// for now they're just going to move up and down the screen
	// Home and End keys have many escape sequences depending on OS
	// [1~ [7~ [H OH for home
	// [4~ [8~ [F OF for end 
	// for now they'll just move left and right
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

void editorDrawRows(struct abuf *ab) {
    int y;
    for(y = 0; y < E.screenrows; y++) {
	// Print welcome message
	if(y == E.screenrows/3) {
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

	abAppend(ab, "\x1b[K", 3);
	// Clears one line at a time when in loop it seems more optimal
	// to clear each line as we redraw
	// K clears a line at a time 0,1,2 analogous to J's arguments 0 default
	if(y < E.screenrows - 1){
	    abAppend(ab, "\r\n", 2);
	}
    }
}

// Render UI to the screen after each keypress
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;
    // Hide the cursor when repainting
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    // Escape sequence starts with an escape character followed by [
    editorDrawRows(&ab);

    char buf[32];
    // Old H command changed to H command with arguments, specifying
    // position we want cursor to move to
    // Add 1 to E.cy and #.cx to convert from 0-index to 1-index of terminal
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    // Show cursor when done h and l are used to turn on and off various
    // terminal features
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
/*** input ***/

void editorMoveCursor(int key) {
    switch (key) {
	case ARROW_LEFT:
	    if(E.cx != 0) E.cx--;
	    break;
	case ARROW_RIGHT:
	    if(E.cx != E.screencols - 1) E.cx++;
	    break;
	case ARROW_UP:
	    if(E.cy != 0) E.cy--;
	    break;
	case ARROW_DOWN:
	    if(E.cy != E.screenrows - 1) E.cy++;
	    break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
	case CTRL_KEY('q'):
	    write(STDOUT_FILENO, "\x1b[2J", 4);
	    write(STDOUT_FILENO, "\x1b[H", 3);
	    // Reposition cursor on exit so 
	    // that characters aren't left on screen
	    exit(0);
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
	    E.cx = E.screencols - 1;
	    break;
	case PAGE_UP:
	case PAGE_DOWN: {
	    int times = E.screenrows;
	    while(times--)
		editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
	    }
	    break;
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();
    // Read 1 byte character from input into c
    while(1) {
	editorRefreshScreen();
	editorProcessKeypress();
    }
    return 0;
}

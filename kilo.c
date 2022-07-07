/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

/*** data ***/

struct termios orig_termios;

/*** terminal ***/

// For error handling
void die(const char *s) {
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) 
	die("tcsetattr");
    // Reset terminal flags to original state and throw away pending input
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) 
	die("tcgetattr");
    atexit(disableRawMode);
    // Read current attributes into raw
    struct termios raw = orig_termios;
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

/*** init ***/

int main() {
    enableRawMode();
    // Read 1 byte character from input into c
    while(1) {
	char c = '\0';
	if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) 
	    die("read");
	if(iscntrl(c)) {
	    printf("%d\r\n", c);
	} else {
	    printf("%d ('%c')\r\n", c, c);
	}
	if (c == 'q') break;
    }
    return 0;
}

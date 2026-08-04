/*** includes ***/
#define _DEFAULT_SOURCE // feature test macros
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
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** Defines ***/

#define MyTextEditor_Version "0.0.1"
#define MyTextEditor_TAB_STOP 8
#define MyTextEditor_QUIT_TIMES 3

// Strips the upper 3 bits of a character, mapping Ctrl+key combos to
// their corresponding control codes (e.g., Ctrl+Q -> 17). This mirrors
// what the terminal does when you hold Ctrl and press a letter.
#define CTRL_KEY(k) ((k) & 0x1f)

// We assign special keys starting at 1000 so they don't collide
// with normal ASCII values (0-127). BACKSPACE is 127 because
// that's the actual ASCII DEL character some terminals send.
enum editorKey
{
    BACKSPACE = 127,
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

// Each token type gets its own highlight category.
// HL_NORMAL is the fallback for anything we don't recognize.
enum editorHighlight
{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

// Bit flags to toggle number and string highlighting per-filetype.
// Using bit shifts so we can OR them together cleanly.
#define HL_HIGHLIGHT_NUMBERS (1 << 0) // bit 0: highlight numeric literals
#define HL_HIGHLIGHT_STRINGS (1 << 1) // bit 1: highlight string literals

/*** data ***/

// Holds all the info needed to do syntax highlighting for a particular
// language — the file extensions it applies to, the keyword lists,
// comment delimiters, and which highlight features are turned on.
struct editorSyntax
{
    char *filetype;                  // name shown in the status bar (e.g., "c")
    char **filematch;                // NULL-terminated array of patterns to match filenames
    char **keywords;                 // NULL-terminated array; trailing '|' means it's a type keyword (kw2)
    char *singleline_comment_start;  // e.g., "//"
    char *multiline_comment_start;   // e.g., "/*"
    char *multiline_comment_end;     // e.g., "*/"
    int flags;                       // bitmask of HL_HIGHLIGHT_* flags
};

// Represents one row of text in the editor. We keep both the raw
// characters and a separate "render" string where tabs are expanded
// to spaces, because the screen needs to know actual column positions.
typedef struct erow
{
    int idx;               // this row's index in the file (0-based)
    int size;              // length of the raw char data (excluding '\0')
    int rsize;             // length of the rendered string
    char *chars;           // the raw character data for this line
    char *render;          // the rendered version (tabs expanded, etc.)
    unsigned char *hl;     // array of highlight types, one per render char
    int hl_open_comment;   // nonzero if this row ends inside an unclosed block comment
} erow;

// Global editor state. Pretty much everything lives here — cursor
// position, scroll offsets, terminal dimensions, all the rows of text,
// the filename, the status message, and the original terminal settings
// so we can restore them on exit.
struct editorConfig
{
    int cx, cy;           // cursor position in the file (cx = column, cy = row)
    int rx;               // rendered x position (accounts for tabs)
    int rowoff;           // which file row is at the top of the screen
    int coloff;           // horizontal scroll offset
    int screenrows;       // how many rows the terminal can display
    int screencols;       // how many columns the terminal can display
    int numrows;          // total number of rows in the file
    erow *row;            // dynamically allocated array of editor rows
    int dirty;            // counts unsaved modifications since last save/open
    char *filename;       // currently open filename (NULL if new file)
    char statusmsg[80];   // short message shown at the bottom
    time_t statusmsg_time; // when the status message was set (auto-clears after 5 sec)
    struct editorSyntax *syntax;     // points to the current syntax rules, or NULL
    struct termios orig_termios; // stores original terminal attributes
};

struct editorConfig E;

/*** filetypes ***/

// File extensions we recognize as C/C++ source. Checked against
// the opened filename to decide which syntax rules to apply.
char *C_HL_exetensions[] = {".c", ".h", ".cpp", NULL};

// Keywords for C syntax highlighting. Regular keywords are stored
// as-is (highlighted as kw1). Type keywords and preprocessor
// directives have a trailing '|' to mark them as kw2 — we strip
// the '|' before comparing but use it to pick the color.
char *C_HL_keywords[] = {"switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union", "typedef", "static", "enum", "class", "case",

                         "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", "time_t|", "#include|", "#define|", "#if|", "#ifdef|", "#ifndef|",
                         "#endif|", "#else|", "#pragma|", NULL};

// The highlight database — one entry per supported language.
// Right now it's just C, but you'd add more entries here for
// other languages (Python, JS, etc.).
struct editorSyntax HLDB[] = {
    {"c",
     C_HL_exetensions,
     C_HL_keywords,
     "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

// Calculates how many entries are in HLDB at compile time.
// This way we don't need to manually update a count when we add languages.
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Prototypes ***/

// Forward declarations — these functions are defined further down but
// are called by code that appears before their definitions.
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

/*
 * die() — Error handler. Clears the screen, prints the error message
 * using perror (which appends the errno description), and exits.
 * Called whenever a system call fails and we can't recover.
 */
void die(const char *s)

{
    write(STDOUT_FILENO, "\x1b[2J", 4); // ESC [2J — clears the entire screen
    write(STDOUT_FILENO, "\x1b[H", 3);  // ESC [H — moves cursor to top-left corner

    perror(s);
    exit(1);
}

/*
 * disableRawMode() — Restores the terminal to its original settings.
 * Registered with atexit() so it runs automatically when the program
 * exits, even if we forget to call it explicitly.
 */
void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

/*
 * enableRawMode() — Switches the terminal from cooked (line-buffered)
 * mode to raw mode so we can read input byte-by-byte and handle
 * special keys ourselves. We save the original settings first so
 * disableRawMode() can put things back the way they were.
 */
void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode); // make sure terminal is restored on any exit path

    struct termios raw = E.orig_termios;

    // Input flags:
    // ~ICRNL  — stop the terminal from translating carriage returns (\r) to newlines (\n)
    // ~IXON   — disable Ctrl-S / Ctrl-Q software flow control
    // ~BRKINT — don't let a break condition send SIGINT
    // ~INPCK  — disable parity checking (not really relevant on modern systems)
    // ~ISTRIP — don't strip the 8th bit of each input byte
    raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);   // disable output post-processing (no automatic \n -> \r\n)
    raw.c_cflag |= (CS8);       // set character size to 8 bits per byte
    // Local flags:
    // ~ECHO   — don't echo typed characters back
    // ~ICANON — read input byte-by-byte instead of line-by-line
    // ~ISIG   — disable Ctrl-C (SIGINT) and Ctrl-Z (SIGTSTP) signals
    // ~IEXTEN — disable Ctrl-V literal input
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;  // read() returns as soon as there's any input
    raw.c_cc[VTIME] = 1; // read() times out after 100ms (1 tenth of a second)

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/*
 * editorReadKey() — Waits for and reads a single keypress from stdin.
 * Regular characters are returned as-is. Escape sequences (arrow keys,
 * Page Up/Down, Home, End, Delete) are parsed and mapped to our
 * editorKey enum values so the rest of the code doesn't have to deal
 * with raw escape sequences.
 */
int editorReadKey()
{
    int nread;
    char c;
    // Keep trying until we successfully read 1 byte
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN) // EAGAIN is normal on timeout, anything else is bad
            die("read");
    }

    // If we got an escape character, try to read the rest of the sequence.
    // Escape sequences look like: ESC [ <code>
    if (c == '\x1b')
    {
        char seq[3];

        // If we can't read the next two bytes, the user just pressed Escape
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            // Sequences like ESC [ 5 ~ (Page Up) — digit followed by tilde
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':           // ESC [1~ — Home (some terminals)
                        return HOME_KEY;
                    case '3':           // ESC [3~ — Delete
                        return DEL_KEY;
                    case '4':           // ESC [4~ — End (some terminals)
                        return END_KEY;
                    case '5':           // ESC [5~ — Page Up
                        return PAGE_UP;
                    case '6':           // ESC [6~ — Page Down
                        return PAGE_DOWN;
                    case '7':           // ESC [7~ — Home (other terminals)
                        return HOME_KEY;
                    case '8':           // ESC [8~ — End (other terminals)
                        return END_KEY;
                    }
                }
            }
            else
            {
                // Arrow keys: ESC [ A/B/C/D
                switch (seq[1])
                {
                case 'A':               // ESC [A — Up arrow
                    return ARROW_UP;
                case 'B':               // ESC [B — Down arrow
                    return ARROW_DOWN;
                case 'C':               // ESC [C — Right arrow
                    return ARROW_RIGHT;
                case 'D':               // ESC [D — Left arrow
                    return ARROW_LEFT;
                case 'H':               // ESC [H — Home
                    return HOME_KEY;
                case 'F':               // ESC [F — End
                    return END_KEY;
                }
            }
        }
        // Some terminals send ESC O H / ESC O F for Home/End
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b'; // unrecognized escape sequence, just return ESC
    }
    else
    {
        return c; // normal character, return it directly
    }
}

/*
 * getCursorPosition() — Fallback method to figure out terminal size.
 * Sends the "report cursor position" escape sequence (ESC [6n), then
 * parses the terminal's response (ESC [rows;colsR) to extract the
 * current cursor row and column.
 *
 * Parameters:
 *   rows — pointer to store the cursor row
 *   cols — pointer to store the cursor column
 * Returns: 0 on success, -1 on failure
 */
int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) // ESC [6n — ask terminal for cursor position
        return -1;

    // Read the response character by character until we hit 'R'
    // The response format is: ESC [ <row> ; <col> R
    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R') // 'R' marks the end of the response
            break;
        i++;
    }

    buf[i] = '\0'; // null-terminate the buffer

    if (buf[0] != '\x1b' || buf[1] != '[') // sanity check: response starts with ESC [
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) // parse "row;col" from the response
        return -1;

    return 0;
}

/*
 * getWindowSize() — Gets the terminal dimensions (rows and columns).
 * Tries ioctl(TIOCGWINSZ) first since it's the fast/easy way. If that
 * fails (some systems don't support it), we fall back to moving the
 * cursor to the bottom-right corner and asking the terminal where it
 * ended up.
 *
 * Parameters:
 *   rows — pointer to store number of rows
 *   cols — pointer to store number of columns
 * Returns: 0 on success, -1 on failure
 */
int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Fallback: move cursor to bottom-right corner using large values
        // ESC [999C moves cursor 999 columns right, ESC [999B moves 999 rows down
        // The terminal clamps these to the actual screen edge
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols); // now read back where the cursor landed
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** Syntax Highlight***/

/*
 * is_separator() — Checks if a character is a "separator" for syntax
 * highlighting purposes. We need this to avoid highlighting parts of
 * words — for example, "printf" contains "int" but we shouldn't
 * highlight those 3 characters as a keyword.
 */
int is_separator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

/*
 * editorUpdateSyntax() — Walks through one row of rendered text and
 * assigns a highlight type (HL_NORMAL, HL_NUMBER, HL_STRING, etc.)
 * to each character. Handles single-line comments, multi-line comments,
 * strings (with escape characters), numbers (including decimals),
 * and two categories of keywords.
 *
 * If a multi-line comment is left open at the end of this row, it
 * propagates into the next row via hl_open_comment. When that state
 * changes, we recursively update the next row too so everything
 * stays consistent.
 *
 * Parameters:
 *   row — pointer to the editor row to update
 */
void editorUpdateSyntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize);       // resize hl array to match rendered length
    memset(row->hl, HL_NORMAL, row->rsize);        // start with everything as HL_NORMAL

    if (E.syntax == NULL) // no syntax rules loaded, nothing to highlight
        return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;  // e.g., "//"
    char *mcs = E.syntax->multiline_comment_start;    // e.g., "/*"
    char *mce = E.syntax->multiline_comment_end;      // e.g., "*/"

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;  // treat beginning of line as if preceded by a separator
    int in_string = 0; // 0 = not in string, otherwise holds the quote char (' or ")
    // Check if previous row left a block comment open — if so, we start inside one
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize)
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // --- Single-line comment check ---
        // If we hit "//", highlight the rest of the line and stop
        if (scs_len && !in_string && !in_comment)
        {
            if (!strncmp(&row->render[i], scs, scs_len))
            {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i); // everything from here to EOL is a comment
                break;
            }
        }

        // --- Multi-line comment check ---
        if (mcs_len && mce_len && !in_string)
        {
            if (in_comment)
            {
                row->hl[i] = HL_MLCOMMENT;
                // Check if we've reached the closing "*/"
                if (!strncmp(&row->render[i], mce, mce_len))
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len); // highlight the closing delimiter too
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1; // treat end of comment as a separator
                    continue;
                }
                else
                {
                    i++; // still inside the comment, keep going
                    continue;
                }
            }
            // Check if we're starting a new block comment with "/*"
            else if (!strncmp(&row->render[i], mcs, mcs_len))
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len); // highlight the opening delimiter
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // --- String highlighting ---
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) // only if string highlighting is enabled
        {
            if (in_string)
            {
                row->hl[i] = HL_STRING;
                // Handle escaped characters inside strings — a backslash
                // means the next character is part of the string no matter what
                if (c == '\\' && i + 1 < row->rsize)
                {
                    row->hl[i + 1] = HL_STRING;
                    i += 2; // skip both the backslash and the escaped char
                    continue;
                }
                if (c == in_string) // closing quote matches the opening quote
                    in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            }
            else
            {
                // Starting a new string literal
                if (c == '"' || c == '\'')
                {
                    in_string = c;       // remember which quote character opened it
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // --- Number highlighting ---
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            // A digit is highlighted if it follows a separator or another digit.
            // A dot is highlighted if the previous char was already a number (decimal point).
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // --- Keyword highlighting ---
        // Only check for keywords right after a separator so we don't match
        // partial words (e.g., "return" inside "noreturn")
        if (prev_sep)
        {
            int j;
            for (j = 0; keywords[j]; j++)
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|'; // trailing '|' means it's a type/secondary keyword
                if (kw2)
                    klen--; // don't include the '|' in the comparison

                // Match the keyword AND make sure it's followed by a separator
                if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen]))
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) // we found and highlighted a keyword
            {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    // If our in_comment state changed from what was recorded, update
    // the flag and re-highlight the next row (since opening/closing a
    // block comment on this line affects everything below it)
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
    {
        editorUpdateSyntax(&E.row[row->idx + 1]); // cascade the change downward
    }
}

/*
 * editorSyntaxToColor() — Maps a highlight type to an ANSI color code.
 * These are standard foreground color codes used in escape sequences
 * like ESC [31m (red), ESC [33m (yellow), etc.
 *
 * Parameters:
 *   hl — the highlight type (one of the HL_* enum values)
 * Returns: ANSI color code integer
 */
int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
    case HL_COMMENT:
    case HL_MLCOMMENT:
        return 36;       // cyan for comments
    case HL_KEYWORD1:
        return 33;       // yellow for control-flow keywords
    case HL_KEYWORD2:
        return 32;       // green for types and preprocessor directives
    case HL_STRING:
        return 35;       // magenta for string literals
    case HL_NUMBER:
        return 31;       // red for numeric literals
    case HL_MATCH:
        return 34;       // blue for search matches
    default:
        return 37;       // white for everything else
    }
}

/*
 * editorSelectSyntaxHighlight() — Looks at the current filename's
 * extension and searches HLDB for a matching syntax entry. If found,
 * sets E.syntax and re-highlights every row in the file. If no match
 * is found, E.syntax stays NULL and no highlighting is applied.
 */
void editorSelectSyntaxHighlight()
{
    E.syntax = NULL;
    if (E.filename == NULL)
        return;

    char *ext = strrchr(E.filename, '.'); // find the last dot in the filename

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i])
        {
            int is_ext = (s->filematch[i][0] == '.'); // does this pattern start with a dot?
            // If it's an extension pattern, compare it against the file extension.
            // If it's a plain string, check if it appears anywhere in the filename.
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i])))
            {
                E.syntax = s;

                // Re-highlight all existing rows with the new syntax rules
                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++)
                {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*** Row operations ***/

/*
 * editorRowCxToRx() — Converts a cursor position in the raw char array (cx)
 * to a rendered position (rx) that accounts for tab stops. Tabs don't take
 * up 1 column — they advance to the next multiple of MyTextEditor_TAB_STOP.
 *
 * Parameters:
 *   row — the editor row
 *   cx  — cursor position in the raw chars
 * Returns: the corresponding rendered x position
 */
int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
        {
            // Jump forward to the next tab stop. The math: we need
            // (TAB_STOP - 1) minus however many columns past the last
            // tab stop we already are, then the rx++ below adds the last one.
            rx += (MyTextEditor_TAB_STOP - 1) - (rx % MyTextEditor_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

/*
 * editorRowRxToCx() — The reverse of editorRowCxToRx(). Given a rendered
 * x position, figures out what raw character index that corresponds to.
 * Used mainly by the search feature to convert a match position in the
 * rendered string back to a cursor position in the raw chars.
 *
 * Parameters:
 *   row — the editor row
 *   rx  — rendered x position
 * Returns: the corresponding raw character index (cx)
 */
int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += (MyTextEditor_TAB_STOP - 1) - (cur_rx % MyTextEditor_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) // we've passed the target rx, so cx is our answer
            return cx;
    }
    return cx;
}

/*
 * editorUpdateRow() — Rebuilds the rendered version of a row from its
 * raw characters. Tabs are expanded to spaces (up to the next tab stop).
 * After rendering, syntax highlighting is also updated.
 *
 * Parameters:
 *   row — the editor row to update
 */
void editorUpdateRow(erow *row)
{
    // First pass: count how many tabs are in the row so we can allocate
    // enough memory. Each tab can expand to up to TAB_STOP characters,
    // but it already "counts" as 1 in row->size, so we need (TAB_STOP-1) extra per tab.
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    // Allocate: original size + extra space for tab expansion + null terminator
    row->render = malloc(row->size + tabs * (MyTextEditor_TAB_STOP - 1) + 1);

    // Second pass: copy characters, expanding tabs to spaces
    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';              // at least one space for the tab
            while (idx % MyTextEditor_TAB_STOP != 0) // fill to next tab stop
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row); // recompute syntax highlighting for this row
}

/*
 * editorInsertRow() — Inserts a new row at position 'at' in the file.
 * Shifts existing rows down using memmove, copies the provided string
 * into the new row, and triggers a render + syntax update.
 *
 * Parameters:
 *   at  — index where the new row should be inserted
 *   s   — the text content for the new row
 *   len — length of the text (not including null terminator)
 */
void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows) // bounds check
        return;

    // Grow the row array by one and shift rows at 'at' and below down
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    // Update the idx field of all shifted rows since their positions changed
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx++;

    E.row[at].idx = at;

    // Copy the string data into the new row
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    // Initialize render fields — editorUpdateRow will fill them in
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

/*
 * editorFreeRow() — Frees all heap-allocated memory belonging to a row
 * (the rendered text, the raw chars, and the highlight array).
 */
void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/*
 * editorDelRow() — Deletes the row at index 'at'. Frees its memory,
 * shifts subsequent rows up with memmove, and updates their idx fields.
 *
 * Parameters:
 *   at — index of the row to delete
 */
void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    // Fix up the idx of all rows that got shifted
    for (int j = at; j < E.numrows - 1; j++)
    {
        E.row[j].idx--;
    }
    E.numrows--;
    E.dirty++;
}

/*
 * editorRowInsertChar() — Inserts a single character into a row at
 * position 'at'. Uses memmove to make space, then triggers a re-render.
 *
 * Parameters:
 *   row — the row to insert into
 *   at  — character index where the new char goes
 *   c   — the character to insert
 */
void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size; // clamp to end of row if out of range
    row->chars = realloc(row->chars, row->size + 2); // +1 for new char, +1 for null terminator
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1); // shift chars right (includes '\0')
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

/*
 * editorRowAppendString() — Appends a string to the end of a row.
 * Used when joining two lines (e.g., backspace at the start of a line
 * merges it with the line above).
 *
 * Parameters:
 *   row — the row to append to
 *   s   — the string to append
 *   len — length of the string
 */
void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1); // make room for the appended string
    memcpy(&row->chars[row->size], s, len);                // copy it right after the existing content
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/*
 * editorRowDelChar() — Deletes the character at position 'at' in a row.
 * Shifts the remaining characters left with memmove.
 *
 * Parameters:
 *   row — the row to delete from
 *   at  — index of the character to delete
 */
void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at); // shift left, includes the '\0'
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** Editor Operations ***/

/*
 * editorInsertChar() — Inserts a character at the current cursor position.
 * If the cursor is on the line past the end of the file, a new blank
 * row is created first.
 */
void editorInsertChar(int c)
{
    if (E.cy == E.numrows) // cursor is on the phantom row after the last line
    {
        editorInsertRow(E.numrows, "", 0); // create a new empty row
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++; // advance cursor past the inserted character
}

/*
 * editorInsertNewline() — Handles the Enter key. If the cursor is at
 * column 0, we just insert a blank line above. Otherwise, we split
 * the current line in two: everything from the cursor onward goes into
 * a new line below, and the current line is truncated.
 */
void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0); // insert blank line above current
    }
    else
    {
        erow *row = &E.row[E.cy];
        // Create a new row below with the text from cursor to end of line
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy]; // re-fetch pointer because realloc in editorInsertRow may have moved it
        row->size = E.cx;
        row->chars[row->size] = '\0'; // truncate the current row at the cursor
        editorUpdateRow(row);
    }
    E.cy++; // move cursor to the new line
    E.cx = 0;
}

/*
 * editorDelChar() — Handles backspace / delete at the current cursor
 * position. If there's a character to the left, delete it. If the cursor
 * is at column 0, merge this line with the one above by appending its
 * contents and deleting this row.
 */
void editorDelChar()
{
    if (E.cy == E.numrows) // nothing to delete past end of file
        return;
    if (E.cx == 0 && E.cy == 0) // nothing before the very first position
        return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1); // delete the character just before the cursor
        E.cx--;
    }
    else
    {
        // Cursor is at column 0 — merge this row into the previous one
        E.cx = E.row[E.cy - 1].size; // cursor goes to end of previous row
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** File i/o ***/

/*
 * editorRowsToString() — Serializes all editor rows into a single
 * heap-allocated string with newlines between them, suitable for
 * writing to a file. The caller is responsible for freeing the buffer.
 *
 * Parameters:
 *   buflen — pointer to store the total length of the resulting string
 * Returns: the concatenated string
 */
char *editorRowsToString(int *buflen)
{
    int totlen = 0;
    int j;
    // Calculate total length: sum of all row sizes plus one newline per row
    for (j = 0; j < E.numrows; j++)
    {
        totlen += E.row[j].size + 1; // +1 for the '\n' we'll add
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf; // walking pointer
    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n'; // add newline at the end of each row
        p++;
    }

    return buf;
}

/*
 * editorOpen() — Opens a file and loads it into the editor, one line
 * at a time. Strips trailing newlines/carriage returns from each line.
 * Also triggers syntax highlighting detection based on the filename.
 *
 * Parameters:
 *   filename — path to the file to open
 */
void editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename); // make our own copy of the filename string

    editorSelectSyntaxHighlight(); // pick syntax rules based on file extension

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;   // getline will allocate the buffer for us
    ssize_t linelen;

    // Read lines until EOF. getline handles buffer allocation and resizing.
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        // Strip trailing newline and carriage return characters
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0; // file was just loaded, no unsaved changes
}

/*
 * editorSave() — Writes the current buffer contents to disk. If no
 * filename has been set yet, prompts the user for one. Uses
 * open + ftruncate + write for atomic-ish saving: truncate the file
 * to the exact length, then write the data.
 */
void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s(ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight(); // might need new syntax rules for the new filename
    }

    int len;
    char *buf = editorRowsToString(&len);

    // O_RDWR — open for reading and writing
    // O_CREAT — create the file if it doesn't exist
    // 0644 — owner can read/write, group and others can only read
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1) // set file size to exactly what we need
        {
            if (write(fd, buf, len) == len) // write the entire buffer
            {
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
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** Find ***/

/*
 * editorFindCallback() — Called on every keypress during an incremental
 * search. Searches through all rows for the query string. Supports
 * forward/backward navigation with arrow keys. Highlights the current
 * match in the file, saving and restoring the original highlighting
 * so search matches don't permanently alter the display.
 *
 * Parameters:
 *   query — the current search string
 *   key   — the key that was just pressed
 */
void editorFindCallback(char *query, int key)
{
    static int last_match = -1; // row index of the last match (-1 = no match yet)
    static int direction = 1;   // 1 = search forward, -1 = search backward

    // We save the highlight state of the matched row so we can restore it
    // when the user moves to a different match or exits the search
    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl)
    {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize); // restore original highlights
        free(saved_hl);
        saved_hl = NULL;
    }

    // Enter or Escape exits the search
    if (key == '\r' || key == '\x1b')
    {
        last_match = -1; // reset for next search
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;   // search forward
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;  // search backward
    }
    else
    {
        last_match = -1;  // new character typed, reset search position
        direction = 1;
    }

    if (last_match == -1)
        direction = 1; // if no previous match, always go forward
    int current = last_match;

    // Loop through all rows, wrapping around the file
    int i;
    for (i = 0; i < E.numrows; i++)
    {
        current += direction;
        // Wrap around: if we go past the end, loop to the beginning and vice versa
        if (current == -1)
            current = E.numrows - 1;
        else if (current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query); // search in the rendered text
        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render); // convert render position to char position
            E.rowoff = E.numrows; // set rowoff past EOF so editorScroll() will scroll to the match

            // Save the current highlighting before we overwrite it with HL_MATCH
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);

            memset(&row->hl[match - row->render], HL_MATCH, strlen(query)); // highlight the matched text
            break;
        }
    }
}

/*
 * editorFind() — Entry point for the search feature (Ctrl-F). Saves the
 * cursor position and scroll state before searching. If the user cancels
 * (presses Escape), the cursor is restored to where it was.
 */
void editorFind()
{
    // Save cursor and scroll state so we can restore on cancel
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);

    if (query)
    {
        free(query); // search confirmed, we don't need the query string anymore
    }
    else
    {
        // User cancelled — restore the original cursor position and scroll
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

// A simple dynamic string buffer used to batch up all our screen writes
// so we can send them to the terminal in a single write() call. This
// avoids flickering that would happen if we wrote each escape sequence
// and character individually.
struct abuf
{
    char *b;   // pointer to the buffer memory
    int len;   // current length of the buffer
};

#define ABUF_INIT {NULL, 0} // initializer for an empty append buffer

/*
 * abAppend() — Appends a string of given length to the append buffer.
 * Uses realloc to grow the buffer as needed.
 *
 * Parameters:
 *   ab  — the append buffer
 *   s   — the string to append
 *   len — number of bytes to append
 */
void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len); // grow the buffer to fit new data

    if (new == NULL)
        return; // allocation failed, silently skip (not great, but simple)
    memcpy(&new[ab->len], s, len); // copy the new data at the end
    ab->b = new;
    ab->len += len;
}

/*
 * abFree() — Frees the memory used by an append buffer.
 */
void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** Output ***/

/*
 * editorScroll() — Adjusts the scroll offsets (rowoff and coloff) so
 * that the cursor is always visible on screen. Also computes the
 * rendered x position (rx) from the raw cursor position (cx).
 */
void editorScroll()
{
    E.rx = 0;

    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx); // compute rendered cursor position
    }

    // Vertical scrolling: if cursor is above the visible area, scroll up
    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    // If cursor is below the visible area, scroll down
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    // Horizontal scrolling: if cursor is left of the visible area, scroll left
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    // If cursor is right of the visible area, scroll right
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

/*
 * editorDrawRows() — Renders all visible rows into the append buffer.
 * For rows beyond the end of the file, draws '~' characters (like vim).
 * When the file is empty, displays a centered welcome message.
 * For rows with actual content, applies syntax highlighting by
 * inserting ANSI color escape sequences around differently-colored
 * segments of text.
 *
 * Parameters:
 *   ab — the append buffer to write into
 */
void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff; // map screen row to file row
        if (filerow >= E.numrows)
        {
            // This screen row is past the end of the file
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                // Show a welcome message roughly 1/3 of the way down, but only if no file is loaded
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "MyTextEditor -- version %s", MyTextEditor_Version);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols; // truncate if wider than screen
                int padding = (E.screencols - welcomelen) / 2; // center the message
                if (padding)
                {
                    abAppend(ab, "~", 1); // leading tilde
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1); // pad with spaces to center
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1); // empty rows get a tilde, like vim
            }
        }
        else
        {
            // This screen row has actual file content to display
            int len = E.row[filerow].rsize - E.coloff; // visible portion of the row
            if (len < 0)
                len = 0; // row is entirely scrolled off to the left
            if (len > E.screencols)
                len = E.screencols; // clamp to screen width
            char *c = &E.row[filerow].render[E.coloff];     // pointer to the visible part of the rendered text
            unsigned char *hl = &E.row[filerow].hl[E.coloff]; // corresponding highlight types
            int current_color = -1; // track the current color so we only emit escape codes on change
            int j;
            for (j = 0; j < len; j++)
            {
                if (iscntrl(c[j]))
                {
                    // Render control characters as readable symbols:
                    // Ctrl-A through Ctrl-Z become '@'+1 through '@'+26 (i.e., 'A' through 'Z')
                    // Anything else becomes '?'
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);  // ESC [7m — enable inverted/reverse video
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);   // ESC [m — reset all attributes
                    if (current_color != -1)
                    {
                        // Restore the syntax color that was active before the control char
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5); // ESC [39m — reset to default foreground color
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {
                    // Highlighted character — only emit a new color code if the color actually changed
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color)
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color); // ESC [<color>m — set foreground color
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5); // reset color at end of row
        }

        abAppend(ab, "\x1b[K", 3); // ESC [K — erase from cursor to end of line (clears old content)
        abAppend(ab, "\r\n", 2);    // move to the start of the next line
    }
}

/*
 * editorDrawStatusBar() — Draws the inverted-color status bar at the
 * bottom of the screen. Left side shows the filename and line count.
 * Right side shows the filetype and current line number.
 *
 * Parameters:
 *   ab — the append buffer to write into
 */
void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4); // ESC [7m — inverted colors (white on black -> black on white)
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);
    // Fill the space between left and right status with spaces, then
    // print the right-aligned status when there's exactly enough room
    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen); // right-aligned info fits exactly
            break;
        }
        else
        {
            abAppend(ab, " ", 1); // fill with spaces
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);  // ESC [m — reset all attributes (back to normal colors)
    abAppend(ab, "\r\n", 2);
}

/*
 * editorDrawMessageBar() — Draws the message bar below the status bar.
 * Shows the status message, but only if it was set less than 5 seconds
 * ago. After 5 seconds it quietly disappears on the next screen refresh.
 *
 * Parameters:
 *   ab — the append buffer to write into
 */
void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3); // ESC [K — clear the line
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols; // truncate if too long for the screen
    if (msglen && time(NULL) - E.statusmsg_time < 5) // only show messages less than 5 seconds old
    {
        abAppend(ab, E.statusmsg, msglen);
    }
}

/*
 * editorRefreshScreen() — Redraws the entire screen. First adjusts
 * scroll, then builds up the complete screen content in an append
 * buffer (to avoid flicker), and finally writes it all in one shot.
 * Hides the cursor during drawing so the user doesn't see it jumping
 * around, then repositions it and shows it again.
 */
void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // ESC [?25l — hide the cursor while we redraw
    abAppend(&ab, "\x1b[H", 3);    // ESC [H — move cursor to the top-left corner (home position)

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Position the cursor at its actual location on screen.
    // Terminal rows/cols are 1-indexed, so we add 1. We subtract the
    // scroll offsets to convert from file coordinates to screen coordinates.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);

    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // ESC [?25h — show the cursor again

    write(STDOUT_FILENO, ab.b, ab.len); // flush the entire buffer to the terminal at once
    abFree(&ab);
}

/*
 * editorSetStatusMessage() — Sets the status bar message using printf-
 * style formatting. Records the current time so the message bar knows
 * when to auto-clear it.
 */
void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL); // timestamp so we can expire the message after 5 seconds
}

/*** Input ***/

/*
 * editorPrompt() — Displays a prompt in the status bar and lets the
 * user type a response. Supports backspace for editing, Escape to
 * cancel (returns NULL), and Enter to confirm (returns the input).
 * An optional callback is invoked on every keypress — this is how
 * incremental search works.
 *
 * Parameters:
 *   prompt   — format string for the prompt (must contain %s for the user's input)
 *   callback — function called after each keypress, or NULL if not needed
 * Returns: the user's input string (caller must free), or NULL if cancelled
 */
char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize); // start with a reasonable buffer size

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf); // show the prompt with current input
        editorRefreshScreen();

        int c = editorReadKey();

        // Handle backspace / delete / Ctrl-H — erase the last character
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b') // Escape — cancel the prompt
        {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if (c == '\r') // Enter — confirm the input
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf; // caller takes ownership of this buffer
            }
        }
        else if (!iscntrl(c) && c < 128) // printable ASCII character
        {
            // Double the buffer if we're running out of space
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
            callback(buf, c); // notify callback of every keypress (for incremental search etc.)
    }
}

/*
 * editorMoveCursor() — Moves the cursor in the given direction. Handles
 * boundary conditions like wrapping from the end of one line to the
 * start of the next, and clamping the cursor to the row length when
 * moving vertically between lines of different lengths.
 *
 * Parameters:
 *   key — one of ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN
 */
void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; // NULL if cursor is past the last row

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            E.cx--;
        }
        else if (E.cy > 0) // at column 0? wrap to end of previous line
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        else if (row && E.cx == row->size) // at end of line? wrap to start of next line
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) // allow scrolling one line past the file (the "phantom" row)
        {
            E.cy++;
        }
        break;
    }

    // After moving vertically, snap the cursor to the end of the new row
    // if the new row is shorter than where the cursor was
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

/*
 * editorProcessKeypress() — The main input dispatcher. Reads one
 * keypress and decides what to do with it: quit, save, find, move
 * the cursor, insert/delete characters, or page up/down. Also
 * manages the "press Ctrl-Q N more times to quit with unsaved changes"
 * safety mechanism.
 */
void editorProcessKeypress()
{
    static int quit_times = MyTextEditor_QUIT_TIMES; // counts down when quitting with unsaved changes

    int c = editorReadKey();

    switch (c)
    {
    case '\r': // Enter key
        editorInsertNewline();
        break;

    case CTRL_KEY('q'): // Ctrl-Q — quit
        if (E.dirty && quit_times > 0)
        {
            // Warn about unsaved changes; user must press Ctrl-Q multiple times to force quit
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return; // return early so quit_times doesn't get reset below
        }
        write(STDOUT_FILENO, "\x1b[2J", 4); // ESC [2J — clear the entire screen before exiting
        write(STDOUT_FILENO, "\x1b[H", 3);  // ESC [H — move cursor home
        exit(0);
        break;

    case CTRL_KEY('s'): // Ctrl-S — save
        editorSave();
        break;

    case HOME_KEY: // Home — move cursor to beginning of line
        E.cx = 0;
        break;

    case END_KEY: // End — move cursor to end of line
        if (E.cy < E.numrows)
        {
            E.cx = E.row[E.cy].size;
        }
        break;

    case CTRL_KEY('f'): // Ctrl-F — find / search
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):  // Ctrl-H is the old-school backspace
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT); // delete key removes the char UNDER the cursor (move right first)
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        // Move cursor to the top or bottom of the visible screen first
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff; // jump to top of visible area
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1; // jump to bottom of visible area
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        // Then simulate pressing up/down arrow for one full screen's worth of rows
        int times = E.screenrows;
        while (times--)
        {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'): // Ctrl-L — traditionally "refresh screen", we just ignore it
    case '\x1b':        // Escape — also ignored
        break;

    default:
        editorInsertChar(c); // anything else is a regular character to insert
        break;
    }

    quit_times = MyTextEditor_QUIT_TIMES; // reset the quit safety counter on any non-Ctrl-Q keypress
}

/*** init ***/

/*
 * initEditor() — Initializes the global editor state to sane defaults.
 * Cursor at top-left, no rows, no file loaded, empty status message.
 * Gets the terminal size and reserves the bottom 2 rows for the
 * status bar and message bar.
 */
void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2; // reserve 2 rows at the bottom for status bar and message bar
}

/*
 * main() — Entry point. Puts the terminal in raw mode, initializes the
 * editor, optionally opens a file from the command line, sets the
 * initial help message, and enters the main loop: refresh the screen,
 * then wait for and process a keypress. Repeat forever.
 */
int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]); // open the file specified on the command line
    }

    editorSetStatusMessage("HELP: Ctrl-S = Save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) // main event loop — runs until the user quits
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
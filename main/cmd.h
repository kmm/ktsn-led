#pragma once
#include <stddef.h>

/* Console commands, independent of where they come from (serial, TCP session, later HTTP).
 * Commands print through a cmd_out_t; cmd_exec runs one at a time across all transports. */

#define CMD_LINE_MAX  512
/* A line of CMD_LINE_MAX chars has at most CMD_LINE_MAX/2 words (word + space), so argv can never truncate. */
#define CMD_MAX_ARGS  (CMD_LINE_MAX / 2 + 2)

/* read_line results below 0 */
#define CMD_READ_CLOSED    (-1)    /* connection closed, idle timeout */
#define CMD_READ_CANCEL    (-2)    /* Ctrl-C (or Ctrl-D on serial) */
#define CMD_READ_TOO_LONG  (-3)    /* the line didn't fit: it was read to its end and dropped */

typedef struct cmd_out {
    void (*write)(struct cmd_out *o, const char *s, size_t n);
    /* Interactive transports (serial console, telnet): read the next input line into buf (no newline), echoing it if
     * the transport needs that. Returns its length or a CMD_READ_ code. NULL where there is no one typing (JSON API). */
    int  (*read_line)(struct cmd_out *o, char *buf, size_t cap);
    void *ctx;
    /* The command line as typed, before splitting (NULL if unknown). Splitting drops the quotes of a "..." word, so
     * commands that take program source read the rest of this line instead of re-joining argv (cmd_source). */
    const char *line;
} cmd_out_t;

void out_printf(cmd_out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void out_puts(cmd_out_t *o, const char *s);          /* appends a newline, like puts */
extern cmd_out_t g_out_stdout;                       /* serial console */

typedef int (*cmd_fn)(cmd_out_t *o, int argc, char **argv);
typedef struct { const char *name, *help; cmd_fn fn; } cmd_def_t;

extern const cmd_def_t g_cmds[];                     /* console.c */
extern const int       g_ncmds;

void cmd_init(void);                                 /* call once at boot, before any transport starts */
void cmd_lock(void);                                 /* held while a command runs; take it to read settings safely */
void cmd_unlock(void);
const cmd_def_t *cmd_find(const char *name);
/* Split `line` in place and run it. Returns the command's result, or -1 for an empty line or unknown command. */
int  cmd_exec(cmd_out_t *o, char *line);
/* The serial REPL's last line as typed (esp_console splits it before our command runs; captured in cmd.c). */
const char *cmd_serial_line(void);

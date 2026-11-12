#include "cmd.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void stdout_write(cmd_out_t *o, const char *s, size_t n) { (void)o; fwrite(s, 1, n, stdout); }

/* Serial line input outside the REPL's line editor (used by multi-line entry while a command runs in the REPL task):
 * polls stdin like the serial API does, echoes what is typed, handles backspace, \r\n counts once. */
static int stdin_read_line(cmd_out_t *o, char *buf, size_t cap)
{
    static bool last_cr;
    (void)o;
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { clearerr(stdin); fflush(stdout); vTaskDelay(1); continue; }
        if (c == 3 || c == 4) { fputs("^C\n", stdout); fflush(stdout); return CMD_READ_CANCEL; }
        if (c == '\n' && last_cr) { last_cr = false; continue; }
        last_cr = c == '\r';
        if (c == '\r' || c == '\n') {
            fputc('\n', stdout); fflush(stdout);
            buf[len] = 0;
            return overflow ? CMD_READ_TOO_LONG : (int)len;
        }
        if (c == 8 || c == 127) { if (len) { len--; fputs("\b \b", stdout); } continue; }
        if (c < 32 && c != '\t') continue;
        if (len < cap - 1) { buf[len++] = (char)c; fputc(c, stdout); } else overflow = true;
    }
}

cmd_out_t g_out_stdout = { .write = stdout_write, .read_line = stdin_read_line };

void out_printf(cmd_out_t *o, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) { o->write(o, buf, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);                   /* long lines, e.g. a program's source */
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    o->write(o, big, (size_t)n);
    free(big);
}

void out_puts(cmd_out_t *o, const char *s)
{
    o->write(o, s, strlen(s));
    o->write(o, "\n", 1);
}

void cmd_init(void)   { if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf); }
void cmd_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void cmd_unlock(void) { xSemaphoreGive(s_lock); }

const cmd_def_t *cmd_find(const char *name)
{
    for (int i = 0; i < g_ncmds; i++)
        if (!strcmp(g_cmds[i].name, name)) return &g_cmds[i];
    return NULL;
}

/* esp_console_split_argv turns a word like "HI THERE") into HI THERE) (quotes dropped), which would break DSL strings
 * in one-line program source. The serial REPL splits inside esp_console_run before our command runs, so the raw line
 * is captured here: main/CMakeLists.txt links with --wrap=esp_console_split_argv. Only the REPL task calls the
 * wrapped symbol (cmd_exec below calls the real one and keeps its own copy), so one static line is enough. */
size_t __real_esp_console_split_argv(char *line, char **argv, size_t argv_size);
static char s_serial_line[CMD_LINE_MAX];
size_t __wrap_esp_console_split_argv(char *line, char **argv, size_t argv_size)
{
    strlcpy(s_serial_line, line, sizeof s_serial_line);
    return __real_esp_console_split_argv(line, argv, argv_size);
}
const char *cmd_serial_line(void) { return s_serial_line; }

int cmd_exec(cmd_out_t *o, char *line)
{
    char **argv = malloc(CMD_MAX_ARGS * sizeof *argv);   /* ~1 KB: keep it off small task stacks */
    char *raw = strdup(line);
    if (!argv || !raw) { free(argv); free(raw); out_puts(o, "out of memory"); return -1; }
    const char *prev = o->line;
    o->line = raw;
    int argc = (int)__real_esp_console_split_argv(line, argv, CMD_MAX_ARGS);
    int r = -1;
    if (argc > 0) {
        const cmd_def_t *d = cmd_find(argv[0]);
        if (!d) out_printf(o, "unknown command '%s' (try: help)\n", argv[0]);
        else { cmd_lock(); r = d->fn(o, argc, argv); cmd_unlock(); }
    }
    o->line = prev;
    free(raw);
    free(argv);
    return r;
}

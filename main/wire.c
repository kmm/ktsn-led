#include "wire.h"
#include <ctype.h>

const char *wire_parse(const char *s, wire_t *w, char *norm)
{
    uint8_t n = 0;
    size_t k = 0;
    w->nw = 0;
    for (; *s; s++) {
        char c = (char)toupper((unsigned char)*s);
        if (c == ' ' || c == '_' || c == ',') {          /* separators: one space, never leading/trailing */
            if (k && norm[k - 1] != ' ') norm[k++] = ' ';
            continue;
        }
        uint8_t sh = c == 'R' ? 16 : c == 'G' ? 8 : c == 'B' ? 0 : c == 'W' ? 24 : c == '0' ? WIRE_ZERO : 0xFE;
        if (sh == 0xFE) return "wire format letters are R G B W and 0 (e.g. RGB, GRB, GRBW, \"BRG WWW\")";
        if (n == WIRE_MAX) return "wire format: at most 12 bytes per pixel";
        w->shift[n++] = sh;
        if (sh == 24) w->nw++;
        norm[k++] = c;
    }
    if (k && norm[k - 1] == ' ') k--;
    norm[k] = 0;
    if (!n) return "wire format is empty (e.g. RGB)";
    w->n = n;
    return NULL;
}

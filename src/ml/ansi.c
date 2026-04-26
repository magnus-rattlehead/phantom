#include "ansi.h"

typedef enum {
    AS_NORMAL,
    AS_ESC,
    AS_CSI,
    AS_OSC,
    AS_OSC_ESC,
} AnsiState;

size_t strip_ansi(const char *src, size_t src_len,
                  char *dst, size_t dst_cap)
{
    if (0 == dst_cap) return 0;

    size_t di = 0;
    AnsiState st = AS_NORMAL;

    for (size_t si = 0; si < src_len; si++) {
        unsigned char c = (unsigned char)src[si];

        switch (st) {
        case AS_NORMAL:
            if (0x1b == c) {
                st = AS_ESC;
            } else if ('\n' == c || '\r' == c) {
                if (di < dst_cap - 1) dst[di++] = (char)c;
            } else if (c < 0x20 || 0x7f == c) {
            } else {
                if (di < dst_cap - 1) dst[di++] = (char)c;
            }
            break;

        case AS_ESC:
            if ('[' == c) {
                st = AS_CSI;
            } else if (']' == c) {
                st = AS_OSC;
            } else {
                st = AS_NORMAL;
            }
            break;

        case AS_CSI:
            if (c >= 0x40 && c <= 0x7e) st = AS_NORMAL;
            break;

        case AS_OSC:
            if (0x07 == c) {
                st = AS_NORMAL;
            } else if (0x1b == c) {
                st = AS_OSC_ESC;
            }
            break;

        case AS_OSC_ESC:
            st = AS_NORMAL;
            break;
        }
    }

    dst[di] = '\0';
    return di;
}

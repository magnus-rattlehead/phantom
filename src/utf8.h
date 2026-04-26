#ifndef PHANTOM_UTF8_H
#define PHANTOM_UTF8_H

#include <stdint.h>

/* Returns the byte index of the start of the UTF-8 codepoint that ends at
 * buf[pos-1].  Walks backward over continuation bytes (high two bits 10).
 * Returns 0 when pos is already 0. */
static inline int utf8_prev_start(const char *buf, int pos)
{
    int i = pos - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0u) == 0x80u)
        i--;
    return i;
}

/* Decodes one UTF-8 codepoint from *s and advances *s past it.
 * Returns U+FFFD on any encoding error; never leaves *s unmoved. */
static inline uint32_t utf8_decode(const unsigned char **s)
{
    uint32_t       cp = 0xFFFDu;
    unsigned char  b0 = *(*s)++;

    if (b0 < 0x80u) {
        cp = b0;
    } else if (b0 >= 0xC2u && b0 < 0xE0u) {
        unsigned char b1 = **s;
        if ((b1 & 0xC0u) == 0x80u) { (*s)++; cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (b1 & 0x3Fu); }
    } else if (b0 >= 0xE0u && b0 < 0xF0u) {
        unsigned char b1 = (*s)[0], b2 = (*s)[1];
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
            (*s) += 2;
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            if (cp < 0x0800u || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
        }
    } else if (b0 >= 0xF0u && b0 < 0xF5u) {
        unsigned char b1 = (*s)[0], b2 = (*s)[1], b3 = (*s)[2];
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
            (*s) += 3;
            cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(b1 & 0x3Fu) << 12)
               | ((uint32_t)(b2 & 0x3Fu) <<  6) | (b3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) cp = 0xFFFDu;
        }
    }
    return cp;
}

#endif /* PHANTOM_UTF8_H */

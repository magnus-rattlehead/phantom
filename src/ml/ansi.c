#include "ansi.h"

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

typedef enum {
    AS_NORMAL,
    AS_ESC,
    AS_CSI,
    AS_OSC,
    AS_OSC_ESC,
} AnsiState;

size_t strip_ansi(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    if (0 == dst_cap)
        return 0;

    size_t si = 0, di = 0;
    AnsiState st = AS_NORMAL;

    while (si < src_len) {
#if defined(__AVX2__)
        /* Fast path: 32-byte chunk all printable ASCII [0x20, 0x7E].
         * cmpgt(0x20, data) fires for bytes <0x20 AND >=0x80 (signed). */
        if (AS_NORMAL == st && si + 32 <= src_len && di + 32 < dst_cap) {
            __m256i data = _mm256_loadu_si256((const __m256i *)(src + si));
            __m256i spec = _mm256_or_si256(
                _mm256_cmpgt_epi8(_mm256_set1_epi8(0x20), data),
                _mm256_cmpeq_epi8(data, _mm256_set1_epi8(0x7f)));
            if (0 == _mm256_movemask_epi8(spec)) {
                _mm256_storeu_si256((__m256i *)(dst + di), data);
                si += 32;
                di += 32;
                continue;
            }
        }
#elif defined(__ARM_NEON)
        /* Fast path: 16-byte chunk all printable ASCII [0x20, 0x7E].
         * Sub 0x20: [0x20,0x7E]->[0x00,0x5E]; anything else >=0x5F. */
        if (AS_NORMAL == st && si + 16 <= src_len && di + 16 < dst_cap) {
            uint8x16_t data = vld1q_u8((const uint8_t *)(src + si));
            uint8x16_t adj = vsubq_u8(data, vdupq_n_u8(0x20u));
            if (vmaxvq_u8(adj) < 0x5fu) {
                vst1q_u8((uint8_t *)(dst + di), data);
                si += 16;
                di += 16;
                continue;
            }
        }
#endif
        unsigned char c = (unsigned char)src[si++];

        switch (st) {
        case AS_NORMAL:
            if (0x1b == c) {
                st = AS_ESC;
            } else if ('\n' == c || '\r' == c) {
                if (di < dst_cap - 1)
                    dst[di++] = (char)c;
            } else if (c < 0x20 || c >= 0x7f) {
                /* dropped */
            } else {
                if (di < dst_cap - 1)
                    dst[di++] = (char)c;
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
            if (c >= 0x40 && c <= 0x7e)
                st = AS_NORMAL;
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

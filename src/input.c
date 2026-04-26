#include "input.h"
#include "pty.h"
#include "terminal.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

static void reset_input_state(Terminal *term)
{
    terminal_clear_selection(term);
    terminal_scroll_bottom(term);
}

/* SDL3 keycodes for printable ASCII equal the ASCII codepoint.
 * On macOS, SDL_EVENT_TEXT_INPUT is not fired for key-repeat events when
 * the system "Press and Hold" accent picker intercepts them.  This helper
 * synthesizes the character so held keys still produce output. */
static char synth_repeat_char(SDL_Keycode sym, SDL_Keymod mod)
{
    if (sym >= 'a' && sym <= 'z')
        return (mod & SDL_KMOD_SHIFT) ? (char)(sym - 'a' + 'A') : (char)sym;
    if (sym == SDLK_SPACE) return ' ';
    /* Digits and unshifted punctuation map directly; skip shifted variants
     * since the resulting character is keyboard-layout-dependent. */
    if (sym >= 0x20 && sym <= 0x7e && !(mod & SDL_KMOD_SHIFT))
        return (char)sym;
    return '\0';
}

void input_handle_event(const SDL_Event *event, Terminal *term, Pty *pty)
{
    if (SDL_EVENT_MOUSE_WHEEL == event->type) {
        terminal_scroll(term, (int)(event->wheel.y * (float)SCROLL_LINES_PER_TICK));
        return;
    }

    if (SDL_EVENT_KEY_DOWN == event->type) {
        SDL_Keymod  mod = event->key.mod;
        SDL_Keycode sym = event->key.key;

        if (mod & SDL_KMOD_SHIFT) {
            if (SDLK_PAGEUP == sym) {
                terminal_scroll(term, terminal_rows(term));
                return;
            }
            if (SDLK_PAGEDOWN == sym) {
                terminal_scroll(term, -terminal_rows(term));
                return;
            }
        }

        if ((mod & SDL_KMOD_GUI) && 'c' == sym) {
            char *text = terminal_get_selected_text(term);
            if (text && *text) SDL_SetClipboardText(text);
            free(text);
            return;
        }

    }

    if (SDL_EVENT_TEXT_INPUT == event->type) {
        reset_input_state(term);
        size_t len = strlen(event->text.text);
        if (len) pty_write(pty, event->text.text, len);
        return;
    }

    if (SDL_EVENT_KEY_DOWN != event->type) return;

    SDL_Keycode sym = event->key.key;
    SDL_Keymod  mod = event->key.mod;

    if ((mod & SDL_KMOD_GUI) && 'v' == sym) {
        char *text = SDL_GetClipboardText();
        if (text && *text) {
            terminal_scroll_bottom(term);
            if (terminal_bracketed_paste(term))
                pty_write(pty, PASTE_START, sizeof(PASTE_START) - 1);
            pty_write(pty, text, strlen(text));
            if (terminal_bracketed_paste(term))
                pty_write(pty, PASTE_END,   sizeof(PASTE_END)   - 1);
        }
        SDL_free(text);
        return;
    }

    char   buf[8];
    size_t len = 0;

    /* Ctrl+letter: generate ASCII control codes 0x01-0x1a */
    if (mod & SDL_KMOD_CTRL) {
        if (sym >= 'a' && sym <= 'z') {
            buf[0] = (char)(sym - 'a' + 1);
            len = 1;
        } else if (sym >= 'A' && sym <= 'Z') {
            buf[0] = (char)(sym - 'A' + 1);
            len = 1;
        } else if ('[' == sym) {
            buf[0] = '\x1b'; len = 1;
        } else if ('\\' == sym) {
            buf[0] = '\x1c'; len = 1;
        } else if (']' == sym) {
            buf[0] = '\x1d'; len = 1;
        }
    }

    if (0 == len) {
        switch (sym) {
        case SDLK_RETURN:
        case SDLK_RETURN2:
            buf[0] = '\r'; len = 1; break;
        case SDLK_BACKSPACE:
            buf[0] = '\x7f'; len = 1; break;
        case SDLK_TAB:
            buf[0] = '\t'; len = 1; break;
        case SDLK_ESCAPE:
            buf[0] = '\x1b'; len = 1; break;
        case SDLK_UP:
            buf[0] = '\x1b';
            buf[1] = terminal_app_cursor_keys(term) ? 'O' : '[';
            buf[2] = 'A'; len = 3; break;
        case SDLK_DOWN:
            buf[0] = '\x1b';
            buf[1] = terminal_app_cursor_keys(term) ? 'O' : '[';
            buf[2] = 'B'; len = 3; break;
        case SDLK_RIGHT:
            buf[0] = '\x1b';
            buf[1] = terminal_app_cursor_keys(term) ? 'O' : '[';
            buf[2] = 'C'; len = 3; break;
        case SDLK_LEFT:
            buf[0] = '\x1b';
            buf[1] = terminal_app_cursor_keys(term) ? 'O' : '[';
            buf[2] = 'D'; len = 3; break;
        case SDLK_HOME:
            buf[0]='\x1b'; buf[1]='['; buf[2]='H'; len = 3; break;
        case SDLK_END:
            buf[0]='\x1b'; buf[1]='['; buf[2]='F'; len = 3; break;
        case SDLK_DELETE:
            buf[0]='\x1b'; buf[1]='['; buf[2]='3'; buf[3]='~'; len = 4; break;
        case SDLK_PAGEUP:
            buf[0]='\x1b'; buf[1]='['; buf[2]='5'; buf[3]='~'; len = 4; break;
        case SDLK_PAGEDOWN:
            buf[0]='\x1b'; buf[1]='['; buf[2]='6'; buf[3]='~'; len = 4; break;
        default: break;
        }
    }

    /* Key-repeat fallback for printable chars: TEXT_INPUT may not fire on
     * macOS when the system accent picker intercepts the repeat. */
    if (0 == len && event->key.repeat
                 && !(mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_GUI)) {
        char ch = synth_repeat_char(sym, mod);
        if (ch) { buf[0] = ch; len = 1; }
    }

    if (len > 0) {
        reset_input_state(term);
        pty_write(pty, buf, len);
    }
}

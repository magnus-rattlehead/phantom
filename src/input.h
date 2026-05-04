#pragma once
#include <SDL3/SDL.h>

typedef struct Terminal Terminal;
typedef struct Pty Pty;

#define PASTE_START "\x1b[200~"
#define PASTE_END "\x1b[201~"

void input_handle_event(const SDL_Event *event, Terminal *term, Pty *pty);

#pragma once
#if defined(__APPLE__)
#include <SDL3/SDL.h>
#include <stdint.h>

/* SDL user-event .code values pushed by native menu/context-menu actions. */
#define MACOS_MENU_COPY 1
#define MACOS_MENU_PASTE 2
#define MACOS_MENU_SELECT_ALL 3
#define MACOS_MENU_NEW_TAB 4
#define MACOS_MENU_CLOSE_TAB 5
#define MACOS_MENU_CLEAR_SCROLLBACK 6
#define MACOS_MENU_FIND 7
#define MACOS_MENU_SPLIT_RIGHT 8
#define MACOS_MENU_SPLIT_DOWN 9
#define MACOS_MENU_CLOSE_PANE 10
#define MACOS_MENU_FOCUS_PREV_PANE 11
#define MACOS_MENU_FOCUS_NEXT_PANE 12

void macos_set_menu_event_type(Uint32 type);

/* bg_rgba: terminal background color packed 0xRRGGBBAA. */
void macos_window_init(SDL_Window *win, uint32_t bg_rgba);

void macos_add_menu_bar(void);

/* has_selection != 0 enables Copy; n_tabs > 1 enables Close Tab;
 * n_panes > 1 enables Close Pane. */
void macos_show_context_menu(SDL_Window *win, int x_pt, int y_pt,
                             int has_selection, int n_tabs, int n_panes);

#endif /* __APPLE__ */

#include "macos.h"
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <stdint.h>

/* Disable the macOS "Press and Hold" accent picker for this process. */
void platform_disable_press_and_hold(void) {
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"ApplePressAndHoldEnabled" : @NO
    }];
}

/* --- Menu infrastructure --- */

static Uint32 s_menu_event_type = 0;

void macos_set_menu_event_type(Uint32 type) { s_menu_event_type = type; }

@interface PhantomMenuHandler : NSObject
@end

@implementation PhantomMenuHandler

- (void)pushCode:(int)code {
    if (0 == s_menu_event_type)
        return;
    SDL_Event e;
    SDL_zero(e);
    e.type = s_menu_event_type;
    e.user.code = code;
    SDL_PushEvent(&e);
}

- (void)menuCopy:(id)sender {
    [self pushCode:MACOS_MENU_COPY];
}
- (void)menuPaste:(id)sender {
    [self pushCode:MACOS_MENU_PASTE];
}
- (void)menuSelectAll:(id)sender {
    [self pushCode:MACOS_MENU_SELECT_ALL];
}
- (void)menuNewTab:(id)sender {
    [self pushCode:MACOS_MENU_NEW_TAB];
}
- (void)menuCloseTab:(id)sender {
    [self pushCode:MACOS_MENU_CLOSE_TAB];
}
- (void)menuClearSB:(id)sender {
    [self pushCode:MACOS_MENU_CLEAR_SCROLLBACK];
}
- (void)menuFind:(id)sender {
    [self pushCode:MACOS_MENU_FIND];
}
- (void)menuSplitRight:(id)sender {
    [self pushCode:MACOS_MENU_SPLIT_RIGHT];
}
- (void)menuSplitDown:(id)sender {
    [self pushCode:MACOS_MENU_SPLIT_DOWN];
}
- (void)menuClosePane:(id)sender {
    [self pushCode:MACOS_MENU_CLOSE_PANE];
}
- (void)menuFocusPrevPane:(id)sender {
    [self pushCode:MACOS_MENU_FOCUS_PREV_PANE];
}
- (void)menuFocusNextPane:(id)sender {
    [self pushCode:MACOS_MENU_FOCUS_NEXT_PANE];
}

@end

static PhantomMenuHandler *s_handler = nil;

static PhantomMenuHandler *handler(void) {
    if (!s_handler)
        s_handler = [[PhantomMenuHandler alloc] init];
    return s_handler;
}

static NSMenuItem *make_item(NSString *title, SEL action, NSString *key,
                             NSEventModifierFlags mod) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key];
    item.target = handler();
    item.keyEquivalentModifierMask = mod;
    return item;
}

/* --- Window init --- */

void macos_window_init(SDL_Window *sdl_win, uint32_t bg_rgba) {
    NSWindow *win = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_win), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        NULL);
    if (!win)
        return;

    win.titlebarAppearsTransparent = YES;
    win.titleVisibility = NSWindowTitleHidden;

    CGFloat r = ((bg_rgba >> 24) & 0xff) / 255.0;
    CGFloat g = ((bg_rgba >> 16) & 0xff) / 255.0;
    CGFloat b = ((bg_rgba >> 8) & 0xff) / 255.0;
    win.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

/* --- Menu bar --- */

void macos_add_menu_bar(void) {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Phantom"];
    [appMenu addItemWithTitle:@"About Phantom"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quitItem =
        [[NSMenuItem alloc] initWithTitle:@"Quit Phantom"
                                   action:@selector(terminate:)
                            keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    appItem.submenu = appMenu;
    [bar addItem:appItem];

    NSMenuItem *shellItem = [[NSMenuItem alloc] initWithTitle:@"Shell"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu *shellMenu = [[NSMenu alloc] initWithTitle:@"Shell"];
    [shellMenu addItem:make_item(@"New Tab", @selector(menuNewTab:), @"t",
                                 NSEventModifierFlagCommand)];
    [shellMenu addItem:make_item(@"Close Tab", @selector(menuCloseTab:), @"w",
                                 NSEventModifierFlagCommand)];
    [shellMenu addItem:[NSMenuItem separatorItem]];
    [shellMenu addItem:make_item(@"Split Vertical", @selector(menuSplitRight:),
                                 @"d", NSEventModifierFlagCommand)];
    [shellMenu
        addItem:make_item(@"Split Horizontal", @selector(menuSplitDown:), @"d",
                          NSEventModifierFlagCommand |
                              NSEventModifierFlagShift)];
    [shellMenu
        addItem:make_item(@"Close Pane", @selector(menuClosePane:), @"", 0)];
    [shellMenu addItem:[NSMenuItem separatorItem]];
    [shellMenu
        addItem:make_item(@"Focus Previous Pane", @selector(menuFocusPrevPane:),
                          @"[", NSEventModifierFlagCommand)];
    [shellMenu
        addItem:make_item(@"Focus Next Pane", @selector(menuFocusNextPane:),
                          @"]", NSEventModifierFlagCommand)];
    shellItem.submenu = shellMenu;
    [bar addItem:shellItem];

    NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@"Edit"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItem:make_item(@"Find", @selector(menuFind:), @"f",
                                NSEventModifierFlagCommand)];
    [editMenu addItem:[NSMenuItem separatorItem]];
    /* Copy/Paste/Select All: no keyboard shortcuts to avoid terminal
     * conflicts (Cmd+C = SIGINT, Cmd+V handled by input layer). */
    [editMenu addItem:make_item(@"Copy", @selector(menuCopy:), @"", 0)];
    [editMenu addItem:make_item(@"Paste", @selector(menuPaste:), @"", 0)];
    [editMenu
        addItem:make_item(@"Select All", @selector(menuSelectAll:), @"", 0)];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:make_item(@"Clear Scrollback", @selector(menuClearSB:),
                                @"k", NSEventModifierFlagCommand)];
    editItem.submenu = editMenu;
    [bar addItem:editItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fsItem =
        [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen"
                                   action:@selector(toggleFullScreen:)
                            keyEquivalent:@"f"];
    fsItem.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [viewMenu addItem:fsItem];
    viewItem.submenu = viewMenu;
    [bar addItem:viewItem];

    NSMenuItem *winItem = [[NSMenuItem alloc] initWithTitle:@"Window"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [winMenu addItemWithTitle:@"Minimize"
                       action:@selector(performMiniaturize:)
                keyEquivalent:@"m"];
    [winMenu addItemWithTitle:@"Zoom"
                       action:@selector(performZoom:)
                keyEquivalent:@""];
    winItem.submenu = winMenu;
    [NSApp setWindowsMenu:winMenu];
    [bar addItem:winItem];

    [NSApp setMainMenu:bar];
}

/* --- Context menu --- */

void macos_show_context_menu(SDL_Window *sdl_win, int x_pt, int y_pt,
                             int has_selection, int n_tabs, int n_panes) {
    NSWindow *win = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_win), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        NULL);
    if (!win)
        return;

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;

    NSMenuItem *copyItem = make_item(@"Copy", @selector(menuCopy:), @"", 0);
    copyItem.enabled = (has_selection != 0);
    [menu addItem:copyItem];

    [menu addItem:make_item(@"Paste", @selector(menuPaste:), @"", 0)];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:make_item(@"Select All", @selector(menuSelectAll:), @"", 0)];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:make_item(@"Clear Scrollback", @selector(menuClearSB:), @"",
                            0)];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:make_item(@"New Tab", @selector(menuNewTab:), @"", 0)];

    NSMenuItem *closeTabItem =
        make_item(@"Close Tab", @selector(menuCloseTab:), @"", 0);
    closeTabItem.enabled = (n_tabs > 1);
    [menu addItem:closeTabItem];

    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:make_item(@"Split Vertical", @selector(menuSplitRight:), @"",
                            0)];
    [menu addItem:make_item(@"Split Horizontal", @selector(menuSplitDown:), @"",
                            0)];

    NSMenuItem *closePaneItem =
        make_item(@"Close Pane", @selector(menuClosePane:), @"", 0);
    closePaneItem.enabled = (n_panes > 1);
    [menu addItem:closePaneItem];

    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:make_item(@"Focus Previous Pane",
                            @selector(menuFocusPrevPane:), @"", 0)];
    [menu addItem:make_item(@"Focus Next Pane", @selector(menuFocusNextPane:),
                            @"", 0)];

    NSView *view = win.contentView;
    /* SDL window coords: (0,0) top-left; NSView (non-flipped): bottom-left. */
    NSPoint pt =
        NSMakePoint((CGFloat)x_pt, view.bounds.size.height - (CGFloat)y_pt);
    [menu popUpMenuPositioningItem:nil atLocation:pt inView:view];
}

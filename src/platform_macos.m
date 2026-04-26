#import <Foundation/Foundation.h>

/* Disable the macOS "Press and Hold" accent picker for this process.
 * registerDefaults: is in-memory only — no disk writes, no effect on other
 * apps, and the user can still override it per-app in System Settings. */
void platform_disable_press_and_hold(void)
{
    [[NSUserDefaults standardUserDefaults]
        registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];
}

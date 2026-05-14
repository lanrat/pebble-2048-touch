// Idle timeout: after 3 minutes of no input we show a warning overlay; if no
// input arrives in the following 30 seconds, the app exits to the watchface.
// Every user interaction (button or touch) should call idle_kick(), which
// both dismisses the warning if visible and resets the timer.
#pragma once
#include <pebble.h>

void idle_init(void);
void idle_deinit(void);

// Reset the inactivity timer. If the warning overlay is showing, also
// dismiss it. The caller is responsible for swallowing the input event that
// triggered the kick when idle_warning_was_visible() returns true (so the
// dismissing press doesn't double as a game move).
void idle_kick(void);

// True if the warning overlay is currently visible.
bool idle_warning_visible(void);

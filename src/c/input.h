// Input handling: touch swipes on touch-capable platforms, button clicks
// elsewhere. Wire input_click_config_provider via
// window_set_click_config_provider in main.c.
#pragma once
#include <pebble.h>

void input_click_config_provider(void *context);

#ifdef PBL_TOUCH
void input_touch_subscribe(void);
void input_touch_unsubscribe(void);
#endif

// 2048 for Pebble. Touch-swipe on touchscreen platforms (emery), buttons
// elsewhere. Board state, score, and high score persist across launches.
//
// Module map:
//   game.{h,c}  — board state, moves, scoring, persistence
//   ui.{h,c}    — layer hierarchy, rendering, reset confirm modal
//   input.{h,c} — touch handler and button click handlers
//   main.c      — app init/deinit and event loop
#include <pebble.h>
#include "game.h"
#include "ui.h"
#include "input.h"

static Window *s_window;

static void init(void) {
  // Time-seed RNG so tile spawns aren't deterministic across launches.
  srand((unsigned)time(NULL));

  game_init();  // loads high score

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_click_config_provider(s_window, input_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = ui_window_load,
    .unload = ui_window_unload,
  });
  window_stack_push(s_window, true);

#ifdef PBL_TOUCH
  input_touch_subscribe();
#endif
}

static void deinit(void) {
#ifdef PBL_TOUCH
  input_touch_unsubscribe();
#endif
  window_destroy(s_window);
  ui_destroy_confirm_window();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

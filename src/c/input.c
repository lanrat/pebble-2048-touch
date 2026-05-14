// Input: touch swipes (on PBL_TOUCH platforms) and button clicks. See input.h.
#include "input.h"
#include "game.h"
#include "ui.h"

// --- Touch swipe handling ---
//
// Translate raw touch events into discrete swipes. We record the start
// position on Touchdown, then on Liftoff compute the delta: the dominant axis
// is the direction, and a threshold filters out taps and tiny movements so
// they don't trigger a move.

#ifdef PBL_TOUCH
static bool s_touch_active;
static int16_t s_touch_x0;
static int16_t s_touch_y0;

static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_active = true;
      s_touch_x0 = event->x;
      s_touch_y0 = event->y;
      break;
    case TouchEvent_Liftoff: {
      if (!s_touch_active) break;  // liftoff without a matching touchdown
      s_touch_active = false;
      if (game_is_over) { game_reset(); break; }
      int16_t dx = event->x - s_touch_x0;
      int16_t dy = event->y - s_touch_y0;
      int16_t adx = dx < 0 ? -dx : dx;
      int16_t ady = dy < 0 ? -dy : dy;
      const int16_t threshold = 18;  // pixels — tune for swipe sensitivity
      if (adx < threshold && ady < threshold) break;
      if (adx > ady) {
        // Horizontal swipe wins.
        if (dx > 0) game_apply_move(game_move_right);
        else        game_apply_move(game_move_left);
      } else {
        // Vertical swipe wins. Note Pebble's y axis grows downward.
        if (dy > 0) game_apply_move(game_move_down);
        else        game_apply_move(game_move_up);
      }
      break;
    }
    case TouchEvent_PositionUpdate:
    default:
      break;
  }
}

// Touch sensor draws power while enabled, so guard with the runtime check —
// older firmware or user setting may disable it.
void input_touch_subscribe(void) {
  if (touch_service_is_enabled()) {
    touch_service_subscribe(touch_handler, NULL);
  }
}
void input_touch_unsubscribe(void) {
  if (touch_service_is_enabled()) {
    touch_service_unsubscribe();
  }
}
#endif  // PBL_TOUCH

// --- Button click handling ---
//
// While game-over banner is showing, any button resets the game.

static void up_click_handler(ClickRecognizerRef r, void *ctx) {
  if (game_is_over) { game_reset(); return; }
  game_apply_move(game_move_up);
}
static void down_click_handler(ClickRecognizerRef r, void *ctx) {
  if (game_is_over) { game_reset(); return; }
  game_apply_move(game_move_down);
}

// SELECT: short = move right, long = show reset confirm.
static void select_short_handler(ClickRecognizerRef r, void *ctx) {
  if (game_is_over) { game_reset(); return; }
  game_apply_move(game_move_right);
}
static void select_long_handler(ClickRecognizerRef r, void *ctx) {
  ui_show_reset_confirm();
}

// BACK: short = move left, long = exit app.
static void back_short_handler(ClickRecognizerRef r, void *ctx) {
  if (game_is_over) { game_reset(); return; }
  game_apply_move(game_move_left);
}
static void back_long_handler(ClickRecognizerRef r, void *ctx) {
  window_stack_pop_all(true);
}

// SELECT and BACK pair a multi_click (count=1, timeout=50ms, last_click_only)
// with a long_click. The multi_click fires on release after a short window,
// not on press — so a held button never triggers BOTH the short action AND
// the long action. Plain single_click_subscribe fires on press, which causes
// that double-fire bug. UP/DOWN don't need this since they have no long
// action.
void input_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_multi_click_subscribe(BUTTON_ID_SELECT, 1, 1, 50, true, select_short_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_handler, NULL);
  window_multi_click_subscribe(BUTTON_ID_BACK, 1, 1, 50, true, back_short_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, 500, back_long_handler, NULL);
}

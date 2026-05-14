// Input: touch swipes (on PBL_TOUCH platforms) and button clicks. See input.h.
//
// Buttons are wired up declaratively via a binding table — see s_bindings
// below. Each binding maps a physical button to a short-press action and an
// optional long-press action.
//
// Click dispatch:
//  - UP / DOWN: single_click. Snappy on-press response. No long action.
//  - SELECT: single_click + long_click. Pebble suppresses single_click on a
//    long press, so short = right move, long = reset overlay.
//  - BACK: multi_click only. The firmware intercepts a long BACK press at
//    the system level and force-exits the app, completely bypassing app-side
//    long_click subscriptions (verified via APP_LOG on real hardware).
//    multi_click(count=1, last_only, timeout=50ms) fires ~50ms AFTER release.
//    For a long press, the firmware force-exits while the user is still
//    holding, so the release event never reaches multi_click and the short
//    action (left move) isn't applied. For a short tap, release happens fast
//    and the handler runs normally.
#include "input.h"
#include "game.h"
#include "ui.h"
#include "idle.h"

// All the discrete actions any button can trigger. Adding a new long-press
// gesture (e.g., long-DOWN = some shortcut) means adding an action here and
// referencing it in s_bindings; no new handler functions required.
typedef enum {
  ACT_NONE = 0,
  ACT_MOVE_UP,
  ACT_MOVE_DOWN,
  ACT_MOVE_LEFT,
  ACT_MOVE_RIGHT,
  ACT_SHOW_RESET_CONFIRM,
} ButtonAction;

typedef struct {
  ButtonId      button;
  ButtonAction  short_action;
  ButtonAction  long_action;  // ACT_NONE → no long_click subscribed
} ButtonBinding;

// Single source of truth for button behavior. Order doesn't matter. BACK's
// long-press exit is handled by the firmware at the system level — we don't
// register a long_action for it.
static const ButtonBinding s_bindings[] = {
  { BUTTON_ID_UP,     ACT_MOVE_UP,    ACT_NONE },
  { BUTTON_ID_DOWN,   ACT_MOVE_DOWN,  ACT_NONE },
  { BUTTON_ID_SELECT, ACT_MOVE_RIGHT, ACT_SHOW_RESET_CONFIRM },
  { BUTTON_ID_BACK,   ACT_MOVE_LEFT,  ACT_NONE },
};
#define NUM_BINDINGS (sizeof(s_bindings) / sizeof(s_bindings[0]))

#define LONG_PRESS_MS 500

static const ButtonBinding *find_binding(ButtonId b) {
  for (size_t i = 0; i < NUM_BINDINGS; i++) {
    if (s_bindings[i].button == b) return &s_bindings[i];
  }
  return NULL;
}

// Dispatch a resolved ButtonAction.
static void dispatch_action(ButtonAction action) {
  switch (action) {
    case ACT_NONE:                                          break;
    case ACT_MOVE_UP:    game_apply_move(game_move_up);     break;
    case ACT_MOVE_DOWN:  game_apply_move(game_move_down);   break;
    case ACT_MOVE_LEFT:  game_apply_move(game_move_left);   break;
    case ACT_MOVE_RIGHT: game_apply_move(game_move_right);  break;
    case ACT_SHOW_RESET_CONFIRM:
      if (!ui_reset_confirm_visible()) ui_show_reset_confirm();
      break;
  }
}

static void short_handler(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  // A press while the inactivity warning is up only dismisses the warning;
  // it must NOT also perform a game action.
  if (idle_warning_visible()) {
    idle_kick();
    return;
  }
  idle_kick();
  // Any short press while the reset-confirm overlay is up either confirms
  // (SELECT) or cancels (others).
  if (ui_reset_confirm_visible()) {
    ui_dismiss_reset_confirm();
    if (b == BUTTON_ID_SELECT) game_reset();
    return;
  }
  if (game_is_over) { game_reset(); return; }
  const ButtonBinding *bind = find_binding(b);
  if (bind) dispatch_action(bind->short_action);
}

static void long_handler(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  if (idle_warning_visible()) {
    idle_kick();
    return;
  }
  idle_kick();
  const ButtonBinding *bind = find_binding(b);
  if (bind) dispatch_action(bind->long_action);
}

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
      // If the inactivity warning is up, kick the idle timer and swallow the
      // touch — by leaving s_touch_active false, the matching liftoff will
      // be ignored and no swipe is interpreted.
      if (idle_warning_visible()) {
        idle_kick();
        s_touch_active = false;
        break;
      }
      idle_kick();
      s_touch_active = true;
      s_touch_x0 = event->x;
      s_touch_y0 = event->y;
      break;
    case TouchEvent_Liftoff: {
      if (!s_touch_active) break;  // liftoff without a matching touchdown
      s_touch_active = false;
      // Any touch dismisses the reset-confirm overlay; confirmation is
      // SELECT-button only.
      if (ui_reset_confirm_visible()) {
        ui_dismiss_reset_confirm();
        break;
      }
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

void input_click_config_provider(void *context) {
  for (size_t i = 0; i < NUM_BINDINGS; i++) {
    const ButtonBinding *b = &s_bindings[i];
    if (b->button == BUTTON_ID_BACK) {
      // See file header for why BACK uses multi_click instead of single_click.
      window_multi_click_subscribe(b->button, 1, 1, 50, true, short_handler);
    } else {
      window_single_click_subscribe(b->button, short_handler);
      if (b->long_action != ACT_NONE) {
        window_long_click_subscribe(b->button, LONG_PRESS_MS,
                                    long_handler, NULL);
      }
    }
  }
}

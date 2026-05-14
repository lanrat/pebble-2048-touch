// Input: touch swipes (on PBL_TOUCH platforms) and button clicks. See input.h.
//
// Buttons are wired up declaratively via a binding table — see s_bindings
// below. Each binding maps a physical button to a short-press action and an
// optional long-press action.
//
// Click dispatch: single_click + (optionally) long_click per button. By
// Pebble convention, when both are subscribed on the same button, single_click
// only fires for presses below the long_click threshold; long_click fires
// when the threshold is crossed and (in that case) single_click does not.
// Some firmware versions still fire single_click on release after long_click
// has fired, so each long handler also sets a per-button latch that the
// short handler checks and clears. The latch is also cleared whenever the
// user interacts with the reset overlay so a stale latch from a long-press
// that opened it doesn't suppress later short presses.
#include "input.h"
#include "game.h"
#include "ui.h"

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
  ACT_EXIT_APP,
} ButtonAction;

typedef struct {
  ButtonId      button;
  ButtonAction  short_action;
  ButtonAction  long_action;  // ACT_NONE → no long press registered
} ButtonBinding;

// Single source of truth for button behavior. Order doesn't matter.
static const ButtonBinding s_bindings[] = {
  { BUTTON_ID_UP,     ACT_MOVE_UP,    ACT_NONE },
  { BUTTON_ID_DOWN,   ACT_MOVE_DOWN,  ACT_NONE },
  { BUTTON_ID_SELECT, ACT_MOVE_RIGHT, ACT_SHOW_RESET_CONFIRM },
  { BUTTON_ID_BACK,   ACT_MOVE_LEFT,  ACT_EXIT_APP },
};
#define NUM_BINDINGS (sizeof(s_bindings) / sizeof(s_bindings[0]))

static const ButtonBinding *find_binding(ButtonId b) {
  for (size_t i = 0; i < NUM_BINDINGS; i++) {
    if (s_bindings[i].button == b) return &s_bindings[i];
  }
  return NULL;
}

// Dispatch a resolved ButtonAction. Returns true if the action consumed the
// event (e.g., showed an overlay or exited). Returns false for game moves so
// the caller can apply the standard "if game_over, reset instead" gating.
static bool dispatch_action(ButtonAction action) {
  switch (action) {
    case ACT_NONE:                                            return true;
    case ACT_MOVE_UP:    game_apply_move(game_move_up);       return false;
    case ACT_MOVE_DOWN:  game_apply_move(game_move_down);     return false;
    case ACT_MOVE_LEFT:  game_apply_move(game_move_left);     return false;
    case ACT_MOVE_RIGHT: game_apply_move(game_move_right);    return false;
    case ACT_SHOW_RESET_CONFIRM:
      if (!ui_reset_confirm_visible()) ui_show_reset_confirm();
      return true;
    case ACT_EXIT_APP:
      // While the overlay is up, BACK-long dismisses it rather than exits.
      if (ui_reset_confirm_visible()) {
        ui_dismiss_reset_confirm();
        return true;
      }
      window_stack_pop_all(true);
      return true;
  }
  return false;
}

#define LONG_PRESS_MS 500

// Per-button latch set by the long handler. The short handler checks and
// clears it on entry. Cleared en masse whenever the user interacts with the
// reset overlay (so a stale latch doesn't suppress later legitimate presses).
static bool s_long_latch[NUM_BUTTONS];

static void clear_all_latches(void) {
  for (int i = 0; i < NUM_BUTTONS; i++) s_long_latch[i] = false;
}

// Belt-and-suspenders disambiguation:
//  - LATCH catches firmware that fires single_click on release after
//    long_click has already fired. Long sets, short clears and bails.
//  - SNAPSHOT/UNDO catches firmware that fires single_click on press before
//    long_click fires. Short takes a snapshot before applying its action;
//    if long fires next, it restores the pre-move state.
// Together they cover both observed firmware behaviors.

static void short_handler(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  if (s_long_latch[b]) { s_long_latch[b] = false; return; }

  if (ui_reset_confirm_visible()) {
    ui_dismiss_reset_confirm();
    clear_all_latches();
    if (b == BUTTON_ID_SELECT) game_reset();
    return;
  }
  if (game_is_over) { game_reset(); return; }

  const ButtonBinding *bind = find_binding(b);
  if (!bind) return;
  // Snapshot for the long_handler to potentially revert if this turns out
  // to be a long press. Skipped for buttons with no long action.
  if (bind->long_action != ACT_NONE) game_snapshot();
  dispatch_action(bind->short_action);
}

static void long_handler(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  // Revert any short action that fired on press, then mark the latch so a
  // trailing single_click on release skips its action.
  game_undo_to_snapshot();
  s_long_latch[b] = true;
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

// Buttons without a long_action use plain single_click (fires on press,
// snappy). Buttons WITH a long_action use multi_click(count=1, last_only)
// which fires on release after a short timeout — that way a held button
// triggers long_click first (at the threshold) and sets the latch, so the
// trailing multi_click on release sees the latch and skips the short action.
// Plain single_click on the same button as long_click in this firmware fires
// on press, applying the short action before long_click can latch.
void input_click_config_provider(void *context) {
  for (size_t i = 0; i < NUM_BINDINGS; i++) {
    const ButtonBinding *b = &s_bindings[i];
    if (b->long_action == ACT_NONE) {
      window_single_click_subscribe(b->button, short_handler);
    } else {
      window_multi_click_subscribe(b->button, 1, 1, 50, true, short_handler);
      window_long_click_subscribe(b->button, LONG_PRESS_MS, long_handler, NULL);
    }
  }
}

// UI: layer hierarchy, board rendering, score/status text, reset confirm
// modal. The game module calls into these to drive redraws and banners.
#pragma once
#include <pebble.h>
#include "game.h"  // for MoveAnim

// Window load/unload for the main game window. Wire these via
// window_set_window_handlers in main.c.
void ui_window_load(Window *window);
void ui_window_unload(Window *window);

// Trigger UI updates after game state changes.
void ui_update_score(void);
void ui_mark_board_dirty(void);

// Kick off a slide animation for the given move. Tiles in `anim->prev_value`
// slide from their source cells to `anim->dest[src]`. ~100ms; the board is
// redrawn from game_board after the animation completes (so a newly spawned
// tile becomes visible at that point). If another animation is in progress
// it is cancelled and snapped to its end state first.
void ui_animate_move(const MoveAnim *anim);

// Show / hide the bottom status banner ("Game over", "You win!", "2048!").
void ui_show_status(const char *text);
void ui_hide_status(void);

// Show the "Reset Game?" overlay on top of the board. Dismissed by
// ui_dismiss_reset_confirm(). While visible, ui_reset_confirm_visible()
// returns true and input.c routes button events to dismiss / confirm
// instead of treating them as game moves.
void ui_show_reset_confirm(void);
void ui_dismiss_reset_confirm(void);
bool ui_reset_confirm_visible(void);

// Inactivity warning overlay. Shown by the idle module after the inactivity
// threshold; dismissed when the user presses a button or touches the screen.
void ui_show_idle_warning(void);
void ui_dismiss_idle_warning(void);

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

// Push the modal "Reset game?" confirmation window. SELECT confirms (calls
// game_reset()); any other button cancels and resumes.
void ui_show_reset_confirm(void);

// Free the confirm window if it was lazily created. Call from deinit.
void ui_destroy_confirm_window(void);

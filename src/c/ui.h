// UI: layer hierarchy, board rendering, score/status text, reset confirm
// modal. The game module calls into these to drive redraws and banners.
#pragma once
#include <pebble.h>

// Window load/unload for the main game window. Wire these via
// window_set_window_handlers in main.c.
void ui_window_load(Window *window);
void ui_window_unload(Window *window);

// Trigger UI updates after game state changes.
void ui_update_score(void);
void ui_mark_board_dirty(void);

// Show / hide the bottom status banner ("Game over", "You win!", "2048!").
void ui_show_status(const char *text);
void ui_hide_status(void);

// Push the modal "Reset game?" confirmation window. SELECT confirms (calls
// game_reset()); any other button cancels and resumes.
void ui_show_reset_confirm(void);

// Free the confirm window if it was lazily created. Call from deinit.
void ui_destroy_confirm_window(void);

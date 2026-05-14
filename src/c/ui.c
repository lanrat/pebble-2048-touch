// UI module: rendering, score/status text, reset confirmation. See ui.h.
#include "ui.h"
#include "game.h"

static Layer *s_board_layer;
static TextLayer *s_score_layer;
static TextLayer *s_status_layer;  // "Game over" / "You win!" / "2048!" banner

// Backing storage for the score TextLayer. TextLayer doesn't copy strings, so
// this buffer must outlive every set_text call.
static char s_score_buf[40];

// Tile background color, indexed by log2 value. The palette ramps from pale
// yellow through orange/red into greens/blues at the high end so the player
// can roughly judge progress at a glance. On B/W displays (aplite, diorite)
// tiles are simply white when empty, black otherwise.
static GColor tile_bg_color(uint8_t v) {
#ifdef PBL_COLOR
  switch (v) {
    case 0:  return GColorLightGray;
    case 1:  return GColorPastelYellow;
    case 2:  return GColorIcterine;
    case 3:  return GColorChromeYellow;
    case 4:  return GColorOrange;
    case 5:  return GColorRed;
    case 6:  return GColorDarkCandyAppleRed;
    case 7:  return GColorYellow;
    case 8:  return GColorIslamicGreen;
    case 9:  return GColorGreen;
    case 10: return GColorBlueMoon;
    case 11: return GColorBlue;
    default: return GColorVividCerulean;
  }
#else
  return (v == 0) ? GColorWhite : GColorBlack;
#endif
}

static GColor tile_text_color(uint8_t v) {
#ifdef PBL_COLOR
  if (v <= 2) return GColorBlack;
  return GColorWhite;
#else
  return (v == 0) ? GColorBlack : GColorWhite;
#endif
}

// Layer update_proc: draws the board. Called by the framework whenever the
// board layer is marked dirty. Cell size is computed from the layer bounds
// each time so we render correctly on every platform (square + round, 144 to
// 200 px wide). Grid is centered in the layer.
static void board_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const int gap = 3;  // pixels between tiles and around the grid
  int avail_w = bounds.size.w;
  int avail_h = bounds.size.h;
  // Use the smaller dimension so the grid stays square on any aspect ratio.
  int side = (avail_w < avail_h) ? avail_w : avail_h;
  int cell = (side - gap * (GRID_N + 1)) / GRID_N;
  int grid = cell * GRID_N + gap * (GRID_N + 1);
  int ox = (bounds.size.w - grid) / 2;
  int oy = (bounds.size.h - grid) / 2;

  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(ox, oy, grid, grid), 4, GCornersAll);

  // Pick a font that fits the cell. Four-digit tiles like "2048" need width.
  GFont font;
  if (cell >= 40) font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  else if (cell >= 28) font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  else font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  for (int r = 0; r < GRID_N; r++) {
    for (int c = 0; c < GRID_N; c++) {
      uint8_t v = game_board[r * GRID_N + c];
      int x = ox + gap + c * (cell + gap);
      int y = oy + gap + r * (cell + gap);
      graphics_context_set_fill_color(ctx, tile_bg_color(v));
      graphics_fill_rect(ctx, GRect(x, y, cell, cell), 3, GCornersAll);
      if (v > 0) {
        char buf[12];  // enough for "65536" plus NUL
        snprintf(buf, sizeof(buf), "%u", (unsigned)(1u << v));
        graphics_context_set_text_color(ctx, tile_text_color(v));
        // Nudge the text down a bit on larger cells so it looks vertically
        // centered — system fonts have noticeable top padding.
        GRect tr = GRect(x, y + (cell > 32 ? 4 : 0), cell, cell);
        graphics_draw_text(ctx, buf, font, tr, GTextOverflowModeFill,
                           GTextAlignmentCenter, NULL);
      }
    }
  }
}

void ui_update_score(void) {
  snprintf(s_score_buf, sizeof(s_score_buf), "%lu / %lu",
           (unsigned long)game_score, (unsigned long)game_high_score);
  text_layer_set_text(s_score_layer, s_score_buf);
}

void ui_mark_board_dirty(void) {
  layer_mark_dirty(s_board_layer);
}

void ui_show_status(const char *text) {
  text_layer_set_text(s_status_layer, text);
  layer_set_hidden(text_layer_get_layer(s_status_layer), false);
}

void ui_hide_status(void) {
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
}

// Build the layer hierarchy and either restore a saved game or start fresh.
// Layout: score row at the top, board fills the middle, status banner at the
// bottom (hidden until game over / 2048).
void ui_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  const int score_h = 20;
  const int status_h = 22;

  s_score_layer = text_layer_create(GRect(0, 0, bounds.size.w, score_h));
  text_layer_set_text_alignment(s_score_layer, GTextAlignmentCenter);
  text_layer_set_font(s_score_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_score_layer, GColorClear);
  text_layer_set_text_color(s_score_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_score_layer));

  GRect board_rect = GRect(0, score_h, bounds.size.w,
                           bounds.size.h - score_h - status_h);
  s_board_layer = layer_create(board_rect);
  layer_set_update_proc(s_board_layer, board_update_proc);
  layer_add_child(root, s_board_layer);

  s_status_layer = text_layer_create(GRect(0, bounds.size.h - status_h,
                                           bounds.size.w, status_h));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_status_layer, GColorClear);
  text_layer_set_text_color(s_status_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_status_layer));
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);

  if (!game_load_persisted()) {
    game_reset();
  }
}

void ui_window_unload(Window *window) {
  text_layer_destroy(s_score_layer);
  text_layer_destroy(s_status_layer);
  layer_destroy(s_board_layer);
}

// --- Reset confirmation modal ---
//
// Pushed on long-press SELECT. SELECT confirms reset, any other button
// dismisses. BACK is intentionally not registered — the system default (pop
// window) already dismisses the prompt without exiting the app.

static Window *s_confirm_window;
static TextLayer *s_confirm_text_layer;

static void confirm_yes_handler(ClickRecognizerRef r, void *ctx) {
  window_stack_pop(true);
  game_reset();
}
static void confirm_no_handler(ClickRecognizerRef r, void *ctx) {
  window_stack_pop(true);
}
static void confirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_yes_handler);
  window_single_click_subscribe(BUTTON_ID_UP, confirm_no_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, confirm_no_handler);
}
static void confirm_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_confirm_text_layer = text_layer_create(b);
  text_layer_set_text(s_confirm_text_layer,
                      "\nReset game?\n\nSELECT: yes\nother: no");
  text_layer_set_text_alignment(s_confirm_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_background_color(s_confirm_text_layer, GColorWhite);
  text_layer_set_text_color(s_confirm_text_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_confirm_text_layer));
}
static void confirm_window_unload(Window *w) {
  text_layer_destroy(s_confirm_text_layer);
}

// Lazy-allocate the confirm window the first time we need it and reuse it
// thereafter. Pushed on top of the game window so the game state is preserved
// underneath while the prompt is up.
void ui_show_reset_confirm(void) {
  if (!s_confirm_window) {
    s_confirm_window = window_create();
    window_set_click_config_provider(s_confirm_window, confirm_click_config);
    window_set_window_handlers(s_confirm_window, (WindowHandlers){
      .load = confirm_window_load,
      .unload = confirm_window_unload,
    });
  }
  window_stack_push(s_confirm_window, true);
}

void ui_destroy_confirm_window(void) {
  if (s_confirm_window) {
    window_destroy(s_confirm_window);
    s_confirm_window = NULL;
  }
}

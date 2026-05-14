// UI module: rendering, score/status text, reset confirmation, slide
// animation. See ui.h.
#include "ui.h"
#include "game.h"

static Layer *s_board_layer;
static TextLayer *s_score_layer;
static TextLayer *s_status_layer;  // "Game over" / "You win!" / "2048!" banner

// Backing storage for the score TextLayer. TextLayer doesn't copy strings, so
// this buffer must outlive every set_text call.
static char s_score_buf[40];

// --- Slide animation state ---
//
// While s_animating is true, board_update_proc renders tiles from
// s_active_anim at interpolated positions instead of drawing from game_board.
// Any newly spawned tile (placed in game_board by game_apply_move) is hidden
// until the animation completes — it isn't in s_active_anim.prev_value, so
// the animation pass simply doesn't draw it. The final redraw on teardown
// switches back to drawing game_board, revealing the spawn.
#define ANIM_DURATION_MS 120

static Animation *s_anim_handle;
static MoveAnim   s_active_anim;
static AnimationProgress s_progress;
static bool s_animating;

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

// Grid geometry. Computed from layer bounds each draw so we render correctly
// on every platform (square + round, 144 to 200 px wide).
typedef struct {
  int gap;
  int cell;       // side length of a single tile in pixels
  int ox, oy;     // top-left of the grid (in layer coords)
} GridGeom;

static GridGeom compute_geom(GRect bounds) {
  GridGeom g;
  g.gap = 3;
  // Use the smaller dimension so the grid stays square on any aspect ratio.
  int side = (bounds.size.w < bounds.size.h) ? bounds.size.w : bounds.size.h;
  g.cell = (side - g.gap * (GRID_N + 1)) / GRID_N;
  int grid = g.cell * GRID_N + g.gap * (GRID_N + 1);
  g.ox = (bounds.size.w - grid) / 2;
  g.oy = (bounds.size.h - grid) / 2;
  return g;
}

// Top-left pixel of cell (r, c) in layer coords.
static GPoint cell_origin(const GridGeom *g, int r, int c) {
  return GPoint(g->ox + g->gap + c * (g->cell + g->gap),
                g->oy + g->gap + r * (g->cell + g->gap));
}

// Linear interpolation: a at p=0, b at p=ANIMATION_NORMALIZED_MAX.
static int lerp_i(int a, int b, AnimationProgress p) {
  return a + ((int32_t)(b - a) * p) / ANIMATION_NORMALIZED_MAX;
}

// Choose a font that fits the cell. Four-digit tiles like "2048" need width.
static GFont font_for_cell(int cell) {
  if (cell >= 40) return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  if (cell >= 28) return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
}

// Draw a single tile at the given top-left pixel. value is the log2 of the
// tile value (0 means empty cell — only the bg gets drawn). Pulled out of the
// rendering loops so static and animated draws share one path.
static void draw_tile(GContext *ctx, const GridGeom *g, GFont font,
                      int x, int y, uint8_t value) {
  graphics_context_set_fill_color(ctx, tile_bg_color(value));
  graphics_fill_rect(ctx, GRect(x, y, g->cell, g->cell), 3, GCornersAll);
  if (value > 0) {
    char buf[12];  // enough for "65536" plus NUL
    snprintf(buf, sizeof(buf), "%u", (unsigned)(1u << value));
    graphics_context_set_text_color(ctx, tile_text_color(value));
    // Nudge text down a bit on larger cells so it looks vertically centered —
    // system fonts have noticeable top padding.
    GRect tr = GRect(x, y + (g->cell > 32 ? 4 : 0), g->cell, g->cell);
    graphics_draw_text(ctx, buf, font, tr, GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
  }
}

// Layer update_proc: draws the board. Called by the framework on dirty marks.
//
// Two modes:
//  - Idle: iterate game_board, draw each cell at its grid position.
//  - Animating: iterate the snapshotted prev_value, draw each non-empty source
//    cell at the interpolated position between its src and dest. The
//    background grid (gray rounded square) and empty-cell placeholders are
//    drawn first so the moving tiles appear "on top" of the static grid.
static void board_update_proc(Layer *layer, GContext *ctx) {
  GridGeom g = compute_geom(layer_get_bounds(layer));
  GFont font = font_for_cell(g.cell);

  int grid_side = g.cell * GRID_N + g.gap * (GRID_N + 1);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(g.ox, g.oy, grid_side, grid_side),
                     4, GCornersAll);

  if (!s_animating) {
    // Idle mode: straight game_board render.
    for (int r = 0; r < GRID_N; r++) {
      for (int c = 0; c < GRID_N; c++) {
        GPoint p = cell_origin(&g, r, c);
        draw_tile(ctx, &g, font, p.x, p.y, game_board[r * GRID_N + c]);
      }
    }
    return;
  }

  // Animating mode. First draw empty-cell backgrounds for every slot so the
  // grid doesn't show through gaps between moving tiles.
  for (int r = 0; r < GRID_N; r++) {
    for (int c = 0; c < GRID_N; c++) {
      GPoint p = cell_origin(&g, r, c);
      draw_tile(ctx, &g, font, p.x, p.y, 0);
    }
  }
  // Then draw each source tile from prev_value at its interpolated position.
  // We deliberately don't draw game_board contents here — the just-spawned
  // tile and post-merge values stay hidden until the animation completes.
  for (int i = 0; i < CELLS; i++) {
    uint8_t v = s_active_anim.prev_value[i];
    if (v == 0) continue;
    int8_t dst = s_active_anim.dest[i];
    int from_r = i / GRID_N, from_c = i % GRID_N;
    int to_r = (dst < 0) ? from_r : (dst / GRID_N);
    int to_c = (dst < 0) ? from_c : (dst % GRID_N);
    GPoint a = cell_origin(&g, from_r, from_c);
    GPoint b = cell_origin(&g, to_r,   to_c);
    int x = lerp_i(a.x, b.x, s_progress);
    int y = lerp_i(a.y, b.y, s_progress);
    draw_tile(ctx, &g, font, x, y, v);
  }
}

// --- Slide animation plumbing ---

static void anim_update(Animation *anim, const AnimationProgress p) {
  s_progress = p;
  layer_mark_dirty(s_board_layer);
}

static void anim_teardown(Animation *anim) {
  s_animating = false;
  s_progress = 0;
  layer_mark_dirty(s_board_layer);  // final pass renders game_board normally
}

static const AnimationImplementation s_anim_impl = {
  .update = anim_update,
  .teardown = anim_teardown,
};

static void anim_stopped(Animation *anim, bool finished, void *ctx) {
  // Pebble docs: after .stopped fires the Animation must be destroyed. We
  // also clear our handle so a follow-up move can schedule a fresh one.
  if (s_anim_handle == anim) s_anim_handle = NULL;
  animation_destroy(anim);
}

void ui_animate_move(const MoveAnim *anim) {
  // If a previous slide is still in flight, cancel it. unschedule fires
  // teardown (which sets s_animating=false and marks the layer dirty); the
  // .stopped handler then destroys it. We immediately overwrite the state
  // below with the new animation.
  if (s_anim_handle) {
    animation_unschedule(s_anim_handle);
    s_anim_handle = NULL;
  }

  s_active_anim = *anim;  // copy: caller's struct lives on the stack
  s_progress = 0;
  s_animating = true;

  Animation *a = animation_create();
  animation_set_duration(a, ANIM_DURATION_MS);
  animation_set_curve(a, AnimationCurveEaseInOut);
  animation_set_implementation(a, &s_anim_impl);
  animation_set_handlers(a, (AnimationHandlers){ .stopped = anim_stopped }, NULL);
  s_anim_handle = a;
  animation_schedule(a);
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
  if (s_anim_handle) {
    animation_unschedule(s_anim_handle);
    s_anim_handle = NULL;
  }
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

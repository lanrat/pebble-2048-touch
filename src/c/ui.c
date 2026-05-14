// UI module: rendering, score/status text, reset confirmation, slide
// animation. See ui.h.
#include "ui.h"
#include "game.h"

static Layer *s_board_layer;
static TextLayer *s_score_layer;
static TextLayer *s_status_layer;  // "Game over" / "You win!" / "2048!" banner
static TextLayer *s_confirm_overlay;  // "Reset Game?" prompt overlay
static bool s_confirm_visible;

// Backing storage for the score TextLayer. TextLayer doesn't copy strings, so
// this buffer must outlive every set_text call.
static char s_score_buf[40];

// --- Move animations ---
//
// A move plays in two phases:
//
//  1. SLIDE (120 ms): tiles are rendered from s_active_anim.prev_value at
//     positions linearly interpolated between their source and destination
//     cells. game_board is hidden in this phase so the merged values and the
//     newly spawned tile don't pop in early.
//  2. POP (150 ms, only if any merges happened): renders game_board normally
//     except that the merged-destination cells are drawn with their rect
//     inflated by a triangular envelope (grow then shrink) to highlight the
//     merge. The newly spawned tile is now visible.
//
// If a fresh move arrives during either phase, the current animation is
// cancelled and a new slide starts immediately from the current game_board
// (the prior move's mutations have already been committed by game.c).
#define SLIDE_MS 120
#define POP_MS   150
#define POP_PIXELS 5  // peak inflate per side for merged tiles

static Animation *s_slide_handle;
static Animation *s_pop_handle;
static MoveAnim   s_active_anim;
static AnimationProgress s_slide_progress;
static bool s_slide_active;
// Cells (post-move) that should pop after the slide. Mirrors merged sources
// in s_active_anim, deduplicated to one flag per destination cell.
static bool s_pop_cells[CELLS];
static int  s_pop_count;
static AnimationProgress s_pop_progress;
static bool s_pop_active;
// Cell index of the newly spawned tile this move, or -1. While the post-slide
// animation runs, the spawn cell is drawn growing from a 0-size point to its
// full size (the rest of the board is drawn from game_board normally).
static int8_t s_spawn_idx = -1;

static void start_post_slide_animation(void);

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
  // gap=4 makes the grid exactly fit the supported display widths (144, 180,
  // 200) with no leftover pixel sliver on the right. Math: (W - 5*gap) must
  // divide evenly by 4; W=200 → cell=45; W=180 → cell=40; W=144 → cell=31.
  g.gap = 4;
  // Use the smaller dimension so the grid stays square on any aspect ratio.
  int side = (bounds.size.w < bounds.size.h) ? bounds.size.w : bounds.size.h;
  g.cell = (side - g.gap * (GRID_N + 1)) / GRID_N;
  int grid = g.cell * GRID_N + g.gap * (GRID_N + 1);
  // Centered horizontally; bottom-aligned vertically. Any leftover vertical
  // space ends up as padding between the score row and the grid.
  g.ox = (bounds.size.w - grid) / 2;
  g.oy = bounds.size.h - grid;
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

// Draw a single tile, inflating the cell rect by `inflate` pixels on each
// side. inflate=0 means a normal-sized tile; positive values are used by the
// merge-pop animation, negative by the spawn fade-in. value is the log2 of
// the tile value (0 means empty — only the bg gets drawn). Set `draw_text`
// false to suppress the number (used during the spawn fade until the box has
// grown enough to fit the number, since Pebble's fonts don't scale).
static void draw_tile_at(GContext *ctx, const GridGeom *g, GFont font,
                         int x, int y, uint8_t value, int inflate,
                         bool draw_text) {
  GRect rect = GRect(x - inflate, y - inflate,
                     g->cell + 2 * inflate, g->cell + 2 * inflate);
  graphics_context_set_fill_color(ctx, tile_bg_color(value));
  graphics_fill_rect(ctx, rect, 3, GCornersAll);
  if (value > 0 && draw_text) {
    char buf[12];  // enough for "65536" plus NUL
    snprintf(buf, sizeof(buf), "%u", (unsigned)(1u << value));
    graphics_context_set_text_color(ctx, tile_text_color(value));
    // Nudge text down a bit on larger cells so it looks vertically centered —
    // system fonts have noticeable top padding.
    GRect tr = GRect(rect.origin.x,
                     rect.origin.y + (g->cell > 32 ? 4 : 0),
                     rect.size.w, rect.size.h);
    graphics_draw_text(ctx, buf, font, tr, GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
  }
}

// During the spawn fade, hold back the number until the box has grown to
// ~70% of full size. Pebble fonts don't scale, so drawing the number in a
// tiny box yields awkward partial glyphs; holding off looks like a clean pop.
#define SPAWN_TEXT_THRESHOLD ((AnimationProgress)(ANIMATION_NORMALIZED_MAX * 7 / 10))

// Triangular envelope: 0 at progress 0 and progress MAX, peak at progress
// MAX/2. Used to compute the per-frame inflate for merge pop.
static int pop_inflate_pixels(void) {
  if (!s_pop_active) return 0;
  AnimationProgress halfway = s_pop_progress < (ANIMATION_NORMALIZED_MAX / 2)
      ? s_pop_progress
      : (ANIMATION_NORMALIZED_MAX - s_pop_progress);
  return (int)(((int32_t)POP_PIXELS * 2 * halfway) / ANIMATION_NORMALIZED_MAX);
}

// Spawn fade-in: tile grows from 0 px to full cell size over the animation.
// inflate=-cell/2 yields a 0-size rect; inflate=0 yields full size.
static int spawn_inflate_pixels(int cell) {
  if (!s_pop_active || s_spawn_idx < 0) return 0;
  int32_t complement = ANIMATION_NORMALIZED_MAX - s_pop_progress;
  return -(int)(((int32_t)cell * complement) / (2 * ANIMATION_NORMALIZED_MAX));
}

// Layer update_proc: draws the board. Called by the framework on dirty marks.
//
// Three modes (mutually exclusive):
//  - Idle: iterate game_board, draw each cell at its grid position.
//  - Sliding: iterate s_active_anim.prev_value, draw each source tile at the
//    interpolated position between its src and dest cells.
//  - Popping: iterate game_board normally, but for merged-destination cells,
//    inflate the tile rect by a time-varying amount (peak in the middle).
static void board_update_proc(Layer *layer, GContext *ctx) {
  GridGeom g = compute_geom(layer_get_bounds(layer));
  GFont font = font_for_cell(g.cell);

  int grid_side = g.cell * GRID_N + g.gap * (GRID_N + 1);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(g.ox, g.oy, grid_side, grid_side),
                     4, GCornersAll);

  if (s_slide_active) {
    // Slide phase: draw empty-cell backgrounds for every slot first so the
    // dark grid doesn't show through gaps between moving tiles.
    for (int r = 0; r < GRID_N; r++) {
      for (int c = 0; c < GRID_N; c++) {
        GPoint p = cell_origin(&g, r, c);
        draw_tile_at(ctx, &g, font, p.x, p.y, 0, 0, true);
      }
    }
    // Then draw each source tile from prev_value at its interpolated
    // position. We deliberately don't draw game_board contents here — the
    // just-spawned tile and post-merge values stay hidden until the slide
    // completes.
    for (int i = 0; i < CELLS; i++) {
      uint8_t v = s_active_anim.prev_value[i];
      if (v == 0) continue;
      int8_t dst = s_active_anim.dest[i];
      int from_r = i / GRID_N, from_c = i % GRID_N;
      int to_r = (dst < 0) ? from_r : (dst / GRID_N);
      int to_c = (dst < 0) ? from_c : (dst % GRID_N);
      GPoint a = cell_origin(&g, from_r, from_c);
      GPoint b = cell_origin(&g, to_r,   to_c);
      int x = lerp_i(a.x, b.x, s_slide_progress);
      int y = lerp_i(a.y, b.y, s_slide_progress);
      draw_tile_at(ctx, &g, font, x, y, v, 0, true);
    }
    return;
  }

  // Idle or post-slide: render game_board. During post-slide, pop cells get
  // a triangular inflate and the spawn cell grows from zero. These are
  // mutually exclusive — spawn lands in an empty cell, merges land in
  // occupied destinations.
  int pop_inflate = pop_inflate_pixels();
  int spawn_inflate = spawn_inflate_pixels(g.cell);
  bool spawn_show_text = !s_pop_active || s_pop_progress >= SPAWN_TEXT_THRESHOLD;
  for (int r = 0; r < GRID_N; r++) {
    for (int c = 0; c < GRID_N; c++) {
      int idx = r * GRID_N + c;
      GPoint p = cell_origin(&g, r, c);
      int this_inflate = 0;
      bool is_spawn = (s_pop_active && idx == s_spawn_idx);
      if (s_pop_active) {
        if (s_pop_cells[idx])        this_inflate = pop_inflate;
        else if (idx == s_spawn_idx) this_inflate = spawn_inflate;
      }
      // If the spawn rect is collapsed (size <= 0), only draw the empty-cell
      // background — drawing the colored tile would emit a stray fill at the
      // cell's center pixel.
      if (is_spawn && (g.cell + 2 * spawn_inflate) <= 0) {
        draw_tile_at(ctx, &g, font, p.x, p.y, 0, 0, true);
        continue;
      }
      // For the spawn cell, draw empty bg first so the growing tile appears
      // "on top" of an empty slot rather than over an instantly-full one.
      if (is_spawn) {
        draw_tile_at(ctx, &g, font, p.x, p.y, 0, 0, true);
      }
      // Suppress the number on the spawn cell until the box is roughly full
      // size — Pebble fonts don't scale, so partial glyphs look bad.
      bool draw_text = is_spawn ? spawn_show_text : true;
      draw_tile_at(ctx, &g, font, p.x, p.y, game_board[idx],
                   this_inflate, draw_text);
    }
  }
}

// --- Slide animation plumbing ---

static void slide_update(Animation *anim, const AnimationProgress p) {
  s_slide_progress = p;
  layer_mark_dirty(s_board_layer);
}

static void slide_teardown(Animation *anim) {
  s_slide_active = false;
  s_slide_progress = 0;
  // Don't kick off the pop here — teardown can run during unschedule (when a
  // new move arrived mid-slide), and we don't want to pop the cancelled
  // slide's merges. .stopped handler decides based on `finished`.
  layer_mark_dirty(s_board_layer);
}

static const AnimationImplementation s_slide_impl = {
  .update = slide_update,
  .teardown = slide_teardown,
};

static void slide_stopped(Animation *anim, bool finished, void *ctx) {
  // Pebble docs: after .stopped fires the Animation must be destroyed.
  if (s_slide_handle == anim) s_slide_handle = NULL;
  animation_destroy(anim);
  // Chain into the post-slide phase only if the slide ran to completion AND
  // something interesting happens there (merge pop or spawn fade). Cancelled
  // slides skip the post phase — the caller is about to start a new slide.
  if (finished && (s_pop_count > 0 || s_spawn_idx >= 0)) {
    start_post_slide_animation();
  }
}

// --- Pop animation plumbing ---

static void pop_update(Animation *anim, const AnimationProgress p) {
  s_pop_progress = p;
  layer_mark_dirty(s_board_layer);
}

static void pop_teardown(Animation *anim) {
  s_pop_active = false;
  s_pop_progress = 0;
  s_pop_count = 0;
  for (int i = 0; i < CELLS; i++) s_pop_cells[i] = false;
  s_spawn_idx = -1;
  layer_mark_dirty(s_board_layer);
}

static const AnimationImplementation s_pop_impl = {
  .update = pop_update,
  .teardown = pop_teardown,
};

static void pop_stopped(Animation *anim, bool finished, void *ctx) {
  if (s_pop_handle == anim) s_pop_handle = NULL;
  animation_destroy(anim);
}

// Drives both the merge pop envelope and the spawn fade-in growth. Both
// effects use s_pop_progress as their time base.
static void start_post_slide_animation(void) {
  s_pop_progress = 0;
  s_pop_active = true;

  Animation *a = animation_create();
  animation_set_duration(a, POP_MS);
  animation_set_curve(a, AnimationCurveLinear);  // envelope is in pop_inflate
  animation_set_implementation(a, &s_pop_impl);
  animation_set_handlers(a, (AnimationHandlers){ .stopped = pop_stopped }, NULL);
  s_pop_handle = a;
  animation_schedule(a);
}

void ui_animate_move(const MoveAnim *anim) {
  // Cancel any in-flight slide or pop. unschedule fires teardown (which
  // clears flags and marks the layer dirty); the .stopped handler then
  // destroys it. The .stopped's `finished=false` ensures the cancelled
  // slide doesn't chain into a pop.
  if (s_slide_handle) { animation_unschedule(s_slide_handle); s_slide_handle = NULL; }
  if (s_pop_handle)   { animation_unschedule(s_pop_handle);   s_pop_handle = NULL; }

  s_active_anim = *anim;  // copy: caller's struct lives on the stack
  s_slide_progress = 0;
  s_slide_active = true;

  // Pre-compute which destination cells need to pop after the slide.
  // Multiple sources can merge into one destination — dedup with the bool.
  for (int i = 0; i < CELLS; i++) s_pop_cells[i] = false;
  s_pop_count = 0;
  for (int i = 0; i < CELLS; i++) {
    if (s_active_anim.merged[i] && s_active_anim.dest[i] >= 0) {
      int d = s_active_anim.dest[i];
      if (!s_pop_cells[d]) {
        s_pop_cells[d] = true;
        s_pop_count++;
      }
    }
  }
  s_spawn_idx = s_active_anim.spawn_idx;

  Animation *a = animation_create();
  animation_set_duration(a, SLIDE_MS);
  animation_set_curve(a, AnimationCurveEaseInOut);
  animation_set_implementation(a, &s_slide_impl);
  animation_set_handlers(a, (AnimationHandlers){ .stopped = slide_stopped }, NULL);
  s_slide_handle = a;
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
// Layout: score row at top (full width), board fills the rest (full width,
// grid bottom-aligned). Status banner is an opaque overlay floating over the
// center of the board area, hidden until game over / 2048.
void ui_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  const int score_h = 22;
  const int status_h = 30;

  s_score_layer = text_layer_create(GRect(0, 0, bounds.size.w, score_h));
  text_layer_set_text_alignment(s_score_layer, GTextAlignmentCenter);
  text_layer_set_font(s_score_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_score_layer, GColorClear);
  text_layer_set_text_color(s_score_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_score_layer));

  // Board fills the full width below the score, all the way to the bottom of
  // the display. compute_geom bottom-aligns the grid within this rect.
  GRect board_rect = GRect(0, score_h, bounds.size.w,
                           bounds.size.h - score_h);
  s_board_layer = layer_create(board_rect);
  layer_set_update_proc(s_board_layer, board_update_proc);
  layer_add_child(root, s_board_layer);

  // Status overlay: opaque band centered vertically over the board area.
  // Added to root after the board layer so it draws on top.
  int status_y = score_h + (bounds.size.h - score_h - status_h) / 2;
  s_status_layer = text_layer_create(GRect(0, status_y,
                                           bounds.size.w, status_h));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_background_color(s_status_layer, GColorWhite);
  text_layer_set_text_color(s_status_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_status_layer));
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);

  // Reset confirm overlay: full-width opaque band centered vertically over
  // the board area, taller than the status banner since it has 4 lines.
  // Added last so it draws above everything else. TextLayer has no native
  // vertical centering — height is sized to the text and the rect itself is
  // positioned so the box's center matches the board area's center.
  const int confirm_h = 110;  // ~4 lines of GOTHIC_24_BOLD
  int confirm_y = score_h + (bounds.size.h - score_h - confirm_h) / 2;
  s_confirm_overlay = text_layer_create(GRect(0, confirm_y,
                                              bounds.size.w, confirm_h));
  text_layer_set_text(s_confirm_overlay,
                      "Reset Game?\n\nSELECT: yes\nother: no");
  text_layer_set_text_alignment(s_confirm_overlay, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_overlay,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_background_color(s_confirm_overlay, GColorWhite);
  text_layer_set_text_color(s_confirm_overlay, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_confirm_overlay));
  layer_set_hidden(text_layer_get_layer(s_confirm_overlay), true);

  if (!game_load_persisted()) {
    game_reset();
  }
}

void ui_window_unload(Window *window) {
  if (s_slide_handle) { animation_unschedule(s_slide_handle); s_slide_handle = NULL; }
  if (s_pop_handle)   { animation_unschedule(s_pop_handle);   s_pop_handle = NULL; }
  text_layer_destroy(s_score_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_confirm_overlay);
  layer_destroy(s_board_layer);
}

// --- Reset confirmation overlay ---
//
// Floats on top of the board (added to root after the board layer). While
// visible, input.c gates button events so they dismiss or confirm instead of
// being treated as game moves. The TextLayer and flag are declared near the
// other UI statics at the top of the file.

void ui_show_reset_confirm(void) {
  layer_set_hidden(text_layer_get_layer(s_confirm_overlay), false);
  s_confirm_visible = true;
}

void ui_dismiss_reset_confirm(void) {
  layer_set_hidden(text_layer_get_layer(s_confirm_overlay), true);
  s_confirm_visible = false;
}

bool ui_reset_confirm_visible(void) {
  return s_confirm_visible;
}

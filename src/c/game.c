// Game logic: board, moves, scoring, persistence. See game.h.
#include "game.h"
#include "ui.h"

uint8_t game_board[CELLS];
uint32_t game_score;
uint32_t game_high_score;
bool game_is_over;
bool game_is_won;

// persist_storage keys. Don't reuse numbers across releases — old installs
// may still have the old key written.
#define PERSIST_KEY_HIGH_SCORE 1
#define PERSIST_KEY_BOARD      2
#define PERSIST_KEY_SCORE      3
#define PERSIST_KEY_WON        4

static int empty_count(void) {
  int n = 0;
  for (int i = 0; i < CELLS; i++) if (game_board[i] == 0) n++;
  return n;
}

// Spawn a single tile in a uniformly random empty cell. 90% chance of a "2"
// (log2=1), 10% chance of a "4" (log2=2) — matches the original 2048. Returns
// the cell index of the spawn, or -1 if the board was full.
static int spawn_tile(void) {
  int empties = empty_count();
  if (empties == 0) return -1;
  int target = rand() % empties;
  uint8_t value = ((rand() % 10) == 0) ? 2 : 1;
  int k = 0;
  for (int i = 0; i < CELLS; i++) {
    if (game_board[i] == 0) {
      if (k == target) { game_board[i] = value; return i; }
      k++;
    }
  }
  return -1;
}

// Slide `row` to the left, merging equal adjacent tiles. Returns true if the
// row changed. Also reports per-source-position motion: `dest[i]` is the
// destination position in this row for the tile that started at position i
// (-1 if cell was empty), and `merged[i]` is true if that tile merged into
// another at the destination.
//
// merged_slot[] enforces the 2048 rule that a freshly-merged tile cannot
// merge again in the same move: [2,2,4,_] -> [4,4,_,_], not [8,_,_,_].
static bool compact_row_left(uint8_t *row, int8_t *dest, bool *merged) {
  uint8_t out[GRID_N] = {0, 0, 0, 0};
  int w = 0;
  bool merged_slot[GRID_N] = {false, false, false, false};
  for (int i = 0; i < GRID_N; i++) { dest[i] = -1; merged[i] = false; }

  for (int i = 0; i < GRID_N; i++) {
    if (row[i] == 0) continue;
    if (w > 0 && out[w - 1] == row[i] && !merged_slot[w - 1]) {
      out[w - 1] += 1;  // log2 increment == doubling the tile value
      merged_slot[w - 1] = true;
      game_score += (1UL << out[w - 1]);
      if (out[w - 1] == 11) game_is_won = true;  // 2^11 == 2048
      dest[i] = w - 1;
      merged[i] = true;
    } else {
      out[w++] = row[i];
      dest[i] = w - 1;
    }
  }

  bool changed = false;
  for (int i = 0; i < GRID_N; i++) {
    if (row[i] != out[i]) changed = true;
    row[i] = out[i];
  }
  return changed;
}

// get_*/put_* copy a row or column in/out of the board. Reading into a local
// array lets compact_row_left work in-place regardless of direction.
static void get_row(int r, uint8_t *row) {
  for (int i = 0; i < GRID_N; i++) row[i] = game_board[r * GRID_N + i];
}
static void put_row(int r, const uint8_t *row) {
  for (int i = 0; i < GRID_N; i++) game_board[r * GRID_N + i] = row[i];
}
static void get_col(int c, uint8_t *col) {
  for (int i = 0; i < GRID_N; i++) col[i] = game_board[i * GRID_N + c];
}
static void put_col(int c, const uint8_t *col) {
  for (int i = 0; i < GRID_N; i++) game_board[i * GRID_N + c] = col[i];
}

// Reverse a 4-element array in place. Used to repurpose compact_row_left for
// rightward/downward moves by reversing -> compacting left -> reversing back.
static void reverse4(uint8_t *a) {
  uint8_t t;
  t = a[0]; a[0] = a[3]; a[3] = t;
  t = a[1]; a[1] = a[2]; a[2] = t;
}

bool game_move_left(MoveAnim *anim) {
  bool changed = false;
  for (int r = 0; r < GRID_N; r++) {
    uint8_t row[GRID_N];
    int8_t row_dest[GRID_N];
    bool row_merged[GRID_N];
    get_row(r, row);
    if (compact_row_left(row, row_dest, row_merged)) {
      put_row(r, row);
      changed = true;
    }
    for (int i = 0; i < GRID_N; i++) {
      int src = r * GRID_N + i;
      anim->dest[src] = (row_dest[i] < 0) ? -1 : (r * GRID_N + row_dest[i]);
      anim->merged[src] = row_merged[i];
    }
  }
  return changed;
}

bool game_move_right(MoveAnim *anim) {
  bool changed = false;
  for (int r = 0; r < GRID_N; r++) {
    uint8_t row[GRID_N];
    int8_t row_dest[GRID_N];
    bool row_merged[GRID_N];
    get_row(r, row);
    reverse4(row);
    bool c = compact_row_left(row, row_dest, row_merged);
    reverse4(row);
    if (c) {
      put_row(r, row);
      changed = true;
    }
    // Map original col i -> reversed col (3-i) -> dest in reversed coords
    // -> back to original col (3 - row_dest[3-i]).
    for (int i = 0; i < GRID_N; i++) {
      int rev_i = (GRID_N - 1) - i;
      int8_t rev_dest = row_dest[rev_i];
      int src = r * GRID_N + i;
      anim->dest[src] = (rev_dest < 0)
        ? -1
        : (r * GRID_N + ((GRID_N - 1) - rev_dest));
      anim->merged[src] = row_merged[rev_i];
    }
  }
  return changed;
}

bool game_move_up(MoveAnim *anim) {
  bool changed = false;
  for (int c = 0; c < GRID_N; c++) {
    uint8_t col[GRID_N];
    int8_t col_dest[GRID_N];
    bool col_merged[GRID_N];
    get_col(c, col);
    if (compact_row_left(col, col_dest, col_merged)) {
      put_col(c, col);
      changed = true;
    }
    for (int i = 0; i < GRID_N; i++) {
      int src = i * GRID_N + c;
      anim->dest[src] = (col_dest[i] < 0) ? -1 : (col_dest[i] * GRID_N + c);
      anim->merged[src] = col_merged[i];
    }
  }
  return changed;
}

bool game_move_down(MoveAnim *anim) {
  bool changed = false;
  for (int c = 0; c < GRID_N; c++) {
    uint8_t col[GRID_N];
    int8_t col_dest[GRID_N];
    bool col_merged[GRID_N];
    get_col(c, col);
    reverse4(col);
    bool ch = compact_row_left(col, col_dest, col_merged);
    reverse4(col);
    if (ch) {
      put_col(c, col);
      changed = true;
    }
    for (int i = 0; i < GRID_N; i++) {
      int rev_i = (GRID_N - 1) - i;
      int8_t rev_dest = col_dest[rev_i];
      int src = i * GRID_N + c;
      anim->dest[src] = (rev_dest < 0)
        ? -1
        : (((GRID_N - 1) - rev_dest) * GRID_N + c);
      anim->merged[src] = col_merged[rev_i];
    }
  }
  return changed;
}

// Cheap game-over check: any empty cell means a move is possible. On a full
// board, the player can still move iff two equal tiles touch horizontally or
// vertically (such a pair could be merged by sliding in that direction).
static bool any_moves_available(void) {
  if (empty_count() > 0) return true;
  for (int r = 0; r < GRID_N; r++) {
    for (int c = 0; c < GRID_N; c++) {
      uint8_t v = game_board[r * GRID_N + c];
      if (c + 1 < GRID_N && game_board[r * GRID_N + c + 1] == v) return true;
      if (r + 1 < GRID_N && game_board[(r + 1) * GRID_N + c] == v) return true;
    }
  }
  return false;
}

// Persist the in-progress game so it survives the app closing (or a crash).
// High score is written separately so it survives game-over clearing.
static void save_state(void) {
  persist_write_data(PERSIST_KEY_BOARD, game_board, sizeof(game_board));
  persist_write_int(PERSIST_KEY_SCORE, (int32_t)game_score);
  persist_write_bool(PERSIST_KEY_WON, game_is_won);
}

static void clear_saved_state(void) {
  persist_delete(PERSIST_KEY_BOARD);
  persist_delete(PERSIST_KEY_SCORE);
  persist_delete(PERSIST_KEY_WON);
}

void game_apply_move(bool (*move_fn)(MoveAnim *)) {
  if (game_is_over) return;
  bool was_won = game_is_won;  // rising edge for the "2048!" banner

  // Capture pre-move state for the animation. We snapshot prev_value before
  // move_fn mutates game_board; dest/merged are populated by move_fn.
  MoveAnim anim;
  for (int i = 0; i < CELLS; i++) {
    anim.prev_value[i] = game_board[i];
    anim.dest[i] = -1;
    anim.merged[i] = false;
  }
  anim.spawn_idx = -1;

  bool changed = move_fn(&anim);
  if (changed) {
    anim.spawn_idx = spawn_tile();
    if (game_score > game_high_score) {
      game_high_score = game_score;
      persist_write_int(PERSIST_KEY_HIGH_SCORE, (int32_t)game_high_score);
    }
    ui_update_score();
    ui_animate_move(&anim);  // schedules the slide; redraw happens on completion
    save_state();
  }
  if (!any_moves_available()) {
    game_is_over = true;
    clear_saved_state();
    ui_show_status(game_is_won ? "You win!" : "Game over");
    vibes_double_pulse();
  } else if (changed && game_is_won && !was_won) {
    // First time crossing 2048 this game — celebrate once.
    ui_show_status("2048!");
    vibes_short_pulse();
  }
}

// --- Snapshot/undo support for short-press / long-press disambiguation ---
//
// input.c calls game_snapshot() before a single_click move is applied. If the
// same button later fires a long_click (i.e. the user was actually holding),
// game_undo_to_snapshot() reverts the move so only the long action runs.

static uint8_t  s_snapshot_board[CELLS];
static uint32_t s_snapshot_score;
static bool     s_snapshot_won;
static bool     s_snapshot_valid;
static uint32_t s_snapshot_at_ms;  // wall-clock time when snapshot was taken

// Snapshot freshness window in ms. Long-click fires at 500 ms, so a snapshot
// from the current press is at most ~500 ms old when undo is considered.
// Older snapshots (left over from a prior short press) must not be undone.
#define SNAPSHOT_FRESH_MS 600

static uint32_t now_ms_u32(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000u + ms;
}

void game_snapshot(void) {
  for (int i = 0; i < CELLS; i++) s_snapshot_board[i] = game_board[i];
  s_snapshot_score = game_score;
  s_snapshot_won = game_is_won;
  s_snapshot_valid = true;
  s_snapshot_at_ms = now_ms_u32();
}

void game_undo_to_snapshot(void) {
  if (!s_snapshot_valid) return;
  // Only undo if the snapshot was taken during the CURRENT press. A stale
  // snapshot from an earlier press would revert too far.
  if (now_ms_u32() - s_snapshot_at_ms > SNAPSHOT_FRESH_MS) {
    s_snapshot_valid = false;
    return;
  }
  s_snapshot_valid = false;
  for (int i = 0; i < CELLS; i++) game_board[i] = s_snapshot_board[i];
  game_score = s_snapshot_score;
  game_is_won = s_snapshot_won;
  game_is_over = false;  // we just undid the move that might have ended it
  ui_cancel_animations();
  ui_update_score();
  ui_hide_status();
  ui_mark_board_dirty();
  // Persist the reverted state so a subsequent app exit doesn't keep the
  // briefly-applied move.
  if (empty_count() == CELLS) {
    // Defensive: never happens in practice (snapshot pre-spawn).
    return;
  }
  persist_write_data(PERSIST_KEY_BOARD, game_board, sizeof(game_board));
  persist_write_int(PERSIST_KEY_SCORE, (int32_t)game_score);
  persist_write_bool(PERSIST_KEY_WON, game_is_won);
}

void game_reset(void) {
  for (int i = 0; i < CELLS; i++) game_board[i] = 0;
  game_score = 0;
  game_is_over = false;
  game_is_won = false;
  spawn_tile();
  spawn_tile();
  ui_update_score();
  ui_hide_status();
  ui_mark_board_dirty();
}

bool game_load_persisted(void) {
  if (!persist_exists(PERSIST_KEY_BOARD)) return false;
  int read = persist_read_data(PERSIST_KEY_BOARD, game_board, sizeof(game_board));
  if (read != (int)sizeof(game_board)) return false;
  game_score = (uint32_t)persist_read_int(PERSIST_KEY_SCORE);
  game_is_won = persist_read_bool(PERSIST_KEY_WON);
  game_is_over = false;
  if (empty_count() == CELLS) return false;
  ui_update_score();
  ui_mark_board_dirty();
  return true;
}

void game_init(void) {
  if (persist_exists(PERSIST_KEY_HIGH_SCORE)) {
    game_high_score = (uint32_t)persist_read_int(PERSIST_KEY_HIGH_SCORE);
  }
}

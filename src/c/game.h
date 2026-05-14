// Game logic: board state, moves, scoring, persistence.
//
// Board cells store log2 of the tile value (0 = empty, 1 = "2", 2 = "4", ...,
// 11 = "2048"). This keeps each cell to one byte and makes merges into an
// "increment by 1" rather than "double the value".
#pragma once
#include <pebble.h>

#define GRID_N 4
#define CELLS (GRID_N * GRID_N)

// State exposed to the UI layer for rendering. Do not mutate from outside
// game.c — use the functions below.
extern uint8_t game_board[CELLS];
extern uint32_t game_score;
extern uint32_t game_high_score;
extern bool game_is_over;
extern bool game_is_won;  // sticky: stays true once 2048 has been reached

// Per-tile motion info used to drive slide animations. Populated by each move
// function; consumed by ui_animate_move(). All indices are flat cell indices
// (row * GRID_N + col).
typedef struct {
  uint8_t prev_value[CELLS];  // board value at each cell BEFORE the move
  int8_t  dest[CELLS];        // dest cell for the tile that started here;
                              //   -1 if the source cell was empty
  bool    merged[CELLS];      // true if this source tile merged at dest
} MoveAnim;

// Move primitives. Each mutates game_board in place, fills `anim` with motion
// info, and returns true if the board changed (i.e. the move was "legal").
bool game_move_left(MoveAnim *anim);
bool game_move_right(MoveAnim *anim);
bool game_move_up(MoveAnim *anim);
bool game_move_down(MoveAnim *anim);

// Apply a move and handle all post-move bookkeeping: spawn a tile, update
// score/high score, persist, kick off the slide animation, and detect
// game-over / 2048.
void game_apply_move(bool (*move_fn)(MoveAnim *));

// Start a fresh game (clears saved state). Triggers UI updates.
void game_reset(void);

// Load high score from persist storage. Call once at app start.
void game_init(void);

// Try to restore a saved in-progress game. Returns false if there's no save
// or it's invalid — caller should fall back to game_reset() in that case.
bool game_load_persisted(void);

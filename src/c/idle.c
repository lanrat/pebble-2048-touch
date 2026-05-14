// Idle timeout state machine. See idle.h.
#include "idle.h"
#include "ui.h"

#define IDLE_TIMEOUT_MS    (3 * 60 * 1000)  // 3 minutes to warning
#define WARNING_TIMEOUT_MS (30 * 1000)      // 30 seconds from warning to exit

static AppTimer *s_timer;
static bool s_warning_visible;

static void schedule_idle(void);

static void exit_to_watchface(void *ctx) {
  s_timer = NULL;
  // Popping all windows ends the app and returns to the watchface.
  window_stack_pop_all(true);
}

static void show_warning(void *ctx) {
  s_timer = NULL;
  s_warning_visible = true;
  ui_show_idle_warning();
  s_timer = app_timer_register(WARNING_TIMEOUT_MS, exit_to_watchface, NULL);
}

static void schedule_idle(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  s_timer = app_timer_register(IDLE_TIMEOUT_MS, show_warning, NULL);
}

void idle_init(void) {
  s_warning_visible = false;
  schedule_idle();
}

void idle_deinit(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  s_warning_visible = false;
}

void idle_kick(void) {
  if (s_warning_visible) {
    ui_dismiss_idle_warning();
    s_warning_visible = false;
  }
  schedule_idle();
}

bool idle_warning_visible(void) {
  return s_warning_visible;
}

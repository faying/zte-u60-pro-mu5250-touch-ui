/*
 * ui.h - Application UI built with LVGL widgets.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_UI_H
#define U60PRO_UI_H

/* Hand over argv before ui_create(): a theme switch re-execs this binary with
 * the current tab, scroll position, screen state and exec history (ui_logic.h
 * ui_launch_t). A plain start has none of them. */
void ui_set_launch(int argc, char **argv);

/* Build the initial screen and its widgets. Call once after lv_init(). */
void ui_create(void);

/* Screen-off idle (see main.c): the power key's fd, and pausing the UI's own
 * fast timer (the 50 ms key poll) while the panel is dark. The 1 s refresh
 * timer keeps running. */
int  ui_key_fd(void);
void ui_idle(int dark);
void ui_debug_tap(void);   /* U60_DEVUI_TAPLOG diagnostics */

/* Every touch read goes through this: returns 1 if the press may reach the
 * UI. A dark screen takes no taps; a double tap wakes it when it went off by
 * itself (never after the power key turned it off); the waking gesture and
 * the next 300 ms are swallowed. */
int ui_touch_filter(int pressed, int x, int y);

#endif /* U60PRO_UI_H */

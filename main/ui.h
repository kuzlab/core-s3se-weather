/* LVGL screen for the laundry weather display (SPEC §5).
 *
 * Every function here must be called from outside the LVGL lock: each one
 * takes lvgl_port_lock() itself. LVGL objects are never touched directly by
 * the network task (SPEC §3.2).
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#include "judge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called when the user taps the screen: toggles the graph range and asks for
 * an immediate refetch (SPEC §1.4). */
typedef void (*ui_tap_cb_t)(int new_range_hours);

/* Starts LVGL, registers the display and the touch input, and builds the
 * static layout. Runs the LVGL timer handler on its own task. */
esp_err_t ui_start(esp_lcd_panel_handle_t panel,
                   esp_lcd_panel_io_handle_t io,
                   esp_lcd_touch_handle_t touch);

void ui_set_tap_cb(ui_tap_cb_t cb);

/* Header: place name on the left, "更新 HH:MM" / "失敗 HH:MM" on the right. */
void ui_set_header(const char *place, const char *status);

void ui_set_verdict(verdict_t v);

/* The two summary lines under the banner. Either may be NULL to clear. */
void ui_set_summary(const char *line1, const char *line2);

/* Radar nowcast line. A negative value hides it. */
void ui_set_nowcast(float max_mm_h);

/* Redraws the bar graph. `start` is the index of the slot containing now. */
void ui_set_graph(const hour_slot_t *slots, int n_slots, int start,
                  const judge_config_t *cfg);

int  ui_range_hours(void);
void ui_set_range_hours(int hours);

#ifdef __cplusplus
}
#endif

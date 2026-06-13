#include "power_mgmt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "lvgl.h"

#include "usb/hid_usage_keyboard.h"

static const char *TAG = "power_mgmt";

#define WAKE_BRIGHTNESS_PCT 100   // bsp_display_backlight_on() level

// IO expander #2 (0x44) charge-control bits (per M5Tab5-UserDemo):
// bit7 CHG_EN active-high, bit5 nCHG_QC_EN active-low. The expander powers up
// with outputs low, so charging stays OFF until firmware enables it.
#define CHG_EN_PIN     IO_EXPANDER_PIN_NUM_7
#define CHG_QC_PIN     IO_EXPANDER_PIN_NUM_5

void power_mgmt_charge_enable(void)
{
    esp_io_expander_handle_t ioex = bsp_io_expander1_init();
    if (!ioex) {
        ESP_LOGE(TAG, "io expander #2 unavailable, charger stays off");
        return;
    }
    esp_io_expander_set_dir(ioex, CHG_EN_PIN | CHG_QC_PIN, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_output_mode(ioex, CHG_EN_PIN | CHG_QC_PIN,
                                    IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_set_level(ioex, CHG_EN_PIN, 1);   // charging on
    esp_io_expander_set_level(ioex, CHG_QC_PIN, 0);   // quick-charge negotiation on
    ESP_LOGI(TAG, "battery charging enabled (CHG_EN=1, QC=on)");
}

static lv_obj_t *s_overlay;            // full-screen black tap-catcher
static lv_obj_t *s_lock_hint;          // "Ctrl+Alt+L" hint shown on tap while locked
static lv_timer_t *s_hint_timer;
static volatile bool s_asleep;
static volatile bool s_locked;         // Ctrl+Alt+L lock: only the hotkey unlocks
static volatile uint16_t s_timeout_s = PM_IDLE_TIMEOUT_S;
static volatile int64_t s_last_key_us;  // keyboard activity (not an LVGL indev)

// Seconds since the last input from any source: LVGL tracks touch (indev)
// activity per display; keyboards bypass LVGL so we time them ourselves.
static uint32_t idle_seconds(void)
{
    uint32_t lv_idle_s = lv_display_get_inactive_time(NULL) / 1000;
    uint32_t key_idle_s = (uint32_t)((esp_timer_get_time() - s_last_key_us) / 1000000);
    return lv_idle_s < key_idle_s ? lv_idle_s : key_idle_s;
}

// LVGL-context only (timer/event cbs, or hid task holding bsp_display_lock).
static void do_sleep(bool locked)
{
    if (s_asleep) { s_locked = s_locked || locked; return; }
    s_asleep = true;
    s_locked = locked;
    lv_obj_move_foreground(s_overlay);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lock_hint, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(0);
    ESP_LOGI(TAG, "screen %s", locked ? "locked" : "sleep");
}

static void do_wake(void)
{
    if (!s_asleep) return;
    s_asleep = false;
    s_locked = false;
    lv_obj_add_flag(s_lock_hint, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(WAKE_BRIGHTNESS_PCT);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_display_trigger_activity(NULL);          // reset the idle clock
    s_last_key_us = esp_timer_get_time();
    ESP_LOGI(TAG, "screen wake");
}

static void hint_off_cb(lv_timer_t *t)
{
    if (s_asleep) {
        lv_obj_add_flag(s_lock_hint, LV_OBJ_FLAG_HIDDEN);
        bsp_display_brightness_set(0);
    }
    s_hint_timer = NULL;
}

static void overlay_event_cb(lv_event_t *e)
{
    if (!s_locked) {
        do_wake();  // doze: any tap wakes (the overlay ate it)
        return;
    }
    // Locked: stay locked, briefly show how to unlock.
    lv_obj_remove_flag(s_lock_hint, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(25);
    if (s_hint_timer) lv_timer_reset(s_hint_timer);
    else {
        s_hint_timer = lv_timer_create(hint_off_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_hint_timer, 1);
    }
}

static void idle_check_cb(lv_timer_t *timer)
{
    if (s_asleep || s_timeout_s == 0) return;
    if (idle_seconds() >= s_timeout_s) do_sleep(false);
}

void power_mgmt_init(void)
{
    s_last_key_us = esp_timer_get_time();

    bsp_display_lock(0);
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_overlay, overlay_event_cb, LV_EVENT_PRESSED, NULL);

    s_lock_hint = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_lock_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lock_hint, lv_color_hex(0x808080), 0);
    lv_label_set_text(s_lock_hint, LV_SYMBOL_EYE_CLOSE "  Ctrl+Alt+L");
    lv_obj_center(s_lock_hint);
    lv_obj_add_flag(s_lock_hint, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(idle_check_cb, 1000, NULL);
    bsp_display_unlock();
}

bool power_mgmt_handle_key(uint8_t usage, uint8_t modifiers)
{
    s_last_key_us = esp_timer_get_time();

    bool ctrl = modifiers & (HID_LEFT_CONTROL | HID_RIGHT_CONTROL);
    bool alt  = modifiers & (HID_LEFT_ALT | HID_RIGHT_ALT);
    bool hotkey = ctrl && alt && usage == HID_KEY_L;

    if (s_asleep) {
        // Locked (Ctrl+Alt+L): ONLY the hotkey unlocks — stray keys are
        // swallowed so a cat on the keyboard can't wake it. Doze (idle
        // timeout): any key wakes. The waking key never reaches the shell.
        if (s_locked && !hotkey) return true;
        bsp_display_lock(0);
        do_wake();
        bsp_display_unlock();
        return true;
    }

    if (hotkey) {                               // manual LOCK
        bsp_display_lock(0);
        do_sleep(true);
        bsp_display_unlock();
        return true;
    }
    return false;
}

// Console hook (`sleep`): same path as the idle timer.
void power_mgmt_sleep_now(void)
{
    bsp_display_lock(0);
    do_sleep(false);
    bsp_display_unlock();
}

// Console hook (`lock`): same as Ctrl+Alt+L — only the hotkey unlocks.
void power_mgmt_lock_now(void)
{
    bsp_display_lock(0);
    do_sleep(true);
    bsp_display_unlock();
}

void power_mgmt_set_timeout(uint16_t seconds)
{
    s_timeout_s = seconds;
}

bool power_mgmt_screen_on(void)
{
    return !s_asleep;
}

uint32_t power_mgmt_idle_s(void)
{
    return idle_seconds();
}

void power_mgmt_poweroff(void)
{
    // No bsp_generate_poweroff_signal() in this vendored BSP, so do it by
    // hand: expander #2 (0x44) pin 4 drives the power latch; a few ~50ms
    // high pulses tell the PMIC to cut power.
    esp_io_expander_handle_t ioex = bsp_io_expander1_init();
    if (!ioex) {
        ESP_LOGE(TAG, "io expander #2 unavailable, can't power off");
        return;
    }
    ESP_LOGW(TAG, "powering off");
    esp_io_expander_set_dir(ioex, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_output_mode(ioex, IO_EXPANDER_PIN_NUM_4,
                                    IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    for (int i = 0; i < 3; i++) {
        esp_io_expander_set_level(ioex, IO_EXPANDER_PIN_NUM_4, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_io_expander_set_level(ioex, IO_EXPANDER_PIN_NUM_4, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool power_mgmt_locked(void)
{
    return s_asleep && s_locked;
}

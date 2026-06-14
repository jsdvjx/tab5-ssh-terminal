// Tab5 SSH Terminal — boot sequence:
//   display -> boot anim -> drivers -> assets -> wifi (BLE prov / terminal
//   setup as fallback) -> web config -> HOME PANEL (primary UI). SSH
//   sessions are opened only from the UI (or Ctrl+Alt+1..9 / local shell);
//   the boot terminal stays underneath as fallback + local-shell surface.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "term.h"
#include "term_render.h"
#include "wifi.h"
#include "ble_prov.h"
#include "ssh_client.h"
#include "ssh_keys.h"
#include "hid_keyboard.h"
#include "i2c_keyboard.h"
#include "i18n.h"
#include "settings.h"
#include "security.h"
#include "setup.h"
#include "ui_home.h"
#include "web_config.h"
#include "assets.h"
#include "power_mon.h"
#include "rtc_rx8130.h"
#include "status_bar.h"
#include "ime_filter.h"
#include "ime_bar.h"
#include "power_mgmt.h"
#include "boot_anim.h"
#include "sfx.h"
#include "local_shell.h"
#include "voice_input.h"
#include "nat_tunnel.h"

static const char *TAG = "main";

static settings_t s_settings;
static term_t *s_term;
static volatile bool s_wifi_phase_done;   // past the boot connect loop

static void say(const char *msg)
{
    term_feed(s_term, (const uint8_t *)msg, strlen(msg));
}

static void on_target_changed(int index)
{
    settings_save(&s_settings);
    ui_home_refresh();
}

// Fresh creds from BLE while the device is already up: reconnect ourselves.
static void ble_reconnect_task(void *arg)
{
    wifi_connect_blocking(s_settings.wifi_ssid, s_settings.wifi_pass);
    vTaskDelete(NULL);
}

// Called (from the BLE host task) after BLE creds were saved to NVS.
static void on_ble_creds(void)
{
    if (!s_wifi_phase_done) {
        // app_main is in the setup/connect loop: unblock it, it reconnects
        // with the new creds (setup_wifi returns true after the cancel).
        setup_cancel();
    } else {
        xTaskCreate(ble_reconnect_task, "ble_reconn", 4096, NULL, 4, NULL);
    }
}

// Ctrl+Alt+P: toggle the full-screen home panel. The terminal keeps its
// full width underneath — no more term_set_cols/PTY-resize dance.
static void on_panel_hotkey(void)
{
    ui_home_toggle();
}

// Background Wi-Fi bring-up: keep retrying until we get an IP (the C6 is
// flaky right after boot), then start the web config server once.
static void wifi_boot_task(void *arg)
{
    int backoff_ms = 2000;
    while (wifi_connect_blocking(s_settings.wifi_ssid, s_settings.wifi_pass) != ESP_OK) {
        ESP_LOGW(TAG, "wifi connect failed, retry in %dms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 15000) backoff_ms += 2000;
    }
    web_config_start(&s_settings, ui_home_refresh);
    ESP_LOGI(TAG, "wifi up, web config started");
    // Reverse tunnel: dial the relay so the device's port 80 (and 8080) are
    // reachable at a public subdomain. Loopback proxy -> 127.0.0.1, so it must
    // start after web_config (the thing it exposes) is listening.
    if (s_settings.nat_enabled) nat_tunnel_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Draw buffers go to PSRAM: the BSP default puts 2x72KB in internal DMA
    // RAM, which starves the esp_hosted SDIO mempool (~48KB) and Wi-Fi never
    // comes up. DSI refreshes from a PSRAM framebuffer anyway.
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    // Panel is natively 720x1280 portrait; rotate LVGL to landscape.
    bsp_display_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    bsp_display_unlock();

    sfx_init();          // speaker up first so the typewriter clicks land
    boot_anim_start();   // typewriter logo; boot continues underneath

    // I2C peripherals (bus is up after bsp_display_start: touch shares it).
    rtc_rx8130_init();      // TZ + system clock from RTC if plausible
    power_mon_init();       // optional INA226; status bar hides batt if absent

    s_term = term_create();
    term_render_start(s_term);
    term_set_cols(s_term, TERM_COLS_MAX);   // no boot side panel: full width
    status_bar_init();
    power_mgmt_init();
    power_mgmt_charge_enable();   // CHG_EN defaults off in hardware

    say("[tab5] Tab5 SSH Terminal\r\n");

    // SD asset pack (fonts/emoji) -> PSRAM, then unmount. Must happen before
    // Wi-Fi: SD and the C6 share the SDMMC peripheral.
    assets_load();
    term_redraw(s_term);
    ime_bar_create();           // hidden candidate bar (needs the CJK font)
    // Load settings BEFORE building the home panel so the saved UI language is
    // applied to every label at creation time (i18n_set_lang before ui_home_create).
    settings_load(&s_settings);
    security_init();            // device PIN (gates web + BLE); logs it to serial
    i18n_set_lang((lang_t)s_settings.lang);
    ui_home_create(&s_settings);  // hidden home panel (needs the CJK font too)
    ime_filter_init(s_term);    // pinyin decoder from the SD dictionary
    local_shell_init(s_term, &s_settings);   // Ctrl+Alt+T console
    voice_input_init(&s_settings);           // Ctrl+Alt+V speech-to-text

    // Keyboards first: setup needs them. Keys go to setup until SSH takes over.
    hid_keyboard_start(s_term);     // USB-A external keyboard
    i2c_keyboard_start();           // M5Stack Tab5 Keyboard (Ext.Port1)
    hid_keyboard_set_sink(setup_key_input);
    hid_keyboard_set_hotkey_cb(on_panel_hotkey);

    ssh_keys_init();    // device RSA key: NVS load or background generation
    power_mgmt_set_timeout(s_settings.sleep_timeout_s);

    // The home panel is the primary UI — open it and hand keyboard/SSH over
    // BEFORE the slow Wi-Fi/C6 bring-up. The boot splash holds on top until
    // boot_anim_finish() below, so the terminal is NEVER shown at boot.
    ssh_client_start(s_term, &s_settings, on_target_changed);
    hid_keyboard_set_sink(NULL);    // default: keys -> SSH/terminal
    ime_filter_set_enabled(true);   // Ctrl+Space pinyin in the main phase
    local_shell_enter();            // keyboard fallback until a session opens
    ui_home_set_open(true);
    boot_anim_finish();             // panel is up: fade the splash out

    // Wi-Fi + BLE bring-up (~14s for the C6) happens with the panel already
    // showing. Never block on it: the C6 (firmware 0.0.0) sometimes RPC-times-
    // out before GOT_IP, so connect on a background task that retries forever.
    ESP_ERROR_CHECK(wifi_init());
    s_wifi_phase_done = true;
    wifi_start_status_updates();
    nat_tunnel_init(&s_settings);   // tunnel started later in wifi_boot_task
    ble_prov_init(&s_settings, on_ble_creds);   // needs esp_hosted from wifi_init
    if (s_settings.wifi_ssid[0]) xTaskCreate(wifi_boot_task, "wifi_boot", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "up");
}

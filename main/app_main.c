// Tab5 SSH Terminal — boot sequence:
//   display -> terminal (left 2/3) + panel (right 1/3) -> keyboards
//   -> wifi (scan/pick on first boot) -> web config -> target pick -> ssh

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
#include "ssh_client.h"
#include "hid_keyboard.h"
#include "i2c_keyboard.h"
#include "settings.h"
#include "setup.h"
#include "ui_panel.h"
#include "web_config.h"
#include "assets.h"

static const char *TAG = "main";

static settings_t s_settings;
static term_t *s_term;

static void say(const char *msg)
{
    term_feed(s_term, (const uint8_t *)msg, strlen(msg));
}

static void on_panel_target_tap(int index)
{
    ssh_client_request_switch(index);
}

static void refresh_target_panel(void)
{
    ui_panel_set_targets(&s_settings, s_settings.last_target, on_panel_target_tap);
}

static void on_target_changed(int index)
{
    settings_save(&s_settings);
    refresh_target_panel();
}

// Ctrl+Alt+P: hide/show the side panel, terminal takes the full width.
static void on_panel_hotkey(void)
{
    bool visible = ui_panel_toggle();
    term_set_cols(s_term, visible ? TERM_COLS_PANEL : TERM_COLS_MAX);
    ssh_client_request_resize();
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

    s_term = term_create();
    term_render_start(s_term);
    ui_panel_create();

    say("[tab5] Tab5 SSH Terminal\r\n");

    // SD asset pack (fonts/emoji) -> PSRAM, then unmount. Must happen before
    // Wi-Fi: SD and the C6 share the SDMMC peripheral.
    assets_load();
    term_redraw(s_term);

    // Keyboards first: setup needs them. Keys go to setup until SSH takes over.
    hid_keyboard_start(s_term);     // USB-A external keyboard
    i2c_keyboard_start();           // M5Stack Tab5 Keyboard (Ext.Port1)
    hid_keyboard_set_sink(setup_key_input);
    hid_keyboard_set_hotkey_cb(on_panel_hotkey);

    bool have_saved = settings_load(&s_settings);
    refresh_target_panel();

    say("[tab5] starting Wi-Fi (C6)...\r\n");
    ESP_ERROR_CHECK(wifi_init());
    ui_panel_set_wifi(NULL, NULL, 0);

    bool need_wifi_setup = !have_saved || s_settings.wifi_ssid[0] == 0
                           || setup_offered(s_term, 3000);

    while (true) {
        if (need_wifi_setup) {
            while (!setup_wifi(s_term, &s_settings)) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        say("[tab5] joining Wi-Fi...\r\n");
        if (wifi_connect_blocking(s_settings.wifi_ssid, s_settings.wifi_pass) == ESP_OK) {
            break;
        }
        say("[tab5] Wi-Fi failed — pick a network again\r\n");
        need_wifi_setup = true;
    }
    wifi_start_status_updates();

    char ip[20] = "?";
    wifi_get_ip(ip, sizeof(ip));
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", ip);
    ui_panel_set_url(url);
    web_config_start(&s_settings, refresh_target_panel);

    char line[80];
    snprintf(line, sizeof(line), "[tab5] web config: %s\r\n", url);
    say(line);

    // Choose the SSH target: modal pick when there are several.
    if (s_settings.n_targets > 1) {
        const char *names[MAX_SSH_TARGETS];
        for (int i = 0; i < s_settings.n_targets; i++) {
            const ssh_target_t *t = &s_settings.targets[i];
            names[i] = t->name[0] ? t->name : t->host;
        }
        say("[tab5] tap the SSH target to connect\r\n");
        s_settings.last_target = ui_panel_pick("Connect to...", names, s_settings.n_targets);
        settings_save(&s_settings);
        refresh_target_panel();
    }

    hid_keyboard_set_sink(NULL);    // default: keys -> SSH
    ssh_client_start(s_term, &s_settings, on_target_changed);

    ESP_LOGI(TAG, "up");
}

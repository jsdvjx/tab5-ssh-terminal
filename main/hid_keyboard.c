// USB HID keyboard -> terminal input bytes.
// Uses espressif/usb_host_hid in boot-protocol mode. US layout.
// TODO: typematic key repeat, CapsLock LED, non-US layouts.

#include "hid_keyboard.h"
#include "i2c_keyboard.h"
#include "ssh_client.h"
#include "local_shell.h"
#include "ime_filter.h"
#include "power_mgmt.h"
#include "ui_home.h"
#include "voice_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

static const char *TAG = "hid_kbd";

static term_t *s_term;
static volatile int s_usb_kbd_count;
static hid_key_sink_t s_sink = ssh_client_send;
static hid_hotkey_cb_t s_hotkey_cb;
static hid_hotkey_cb_t s_shell_cb;
volatile uint32_t g_dbg_sink_bytes;

void hid_keyboard_set_sink(hid_key_sink_t sink)
{
    s_sink = sink ? sink : ssh_client_send;
}

void hid_keyboard_set_hotkey_cb(hid_hotkey_cb_t cb)
{
    s_hotkey_cb = cb;
}

void hid_keyboard_set_shell_cb(hid_hotkey_cb_t cb)
{
    s_shell_cb = cb;
}

// HID usage id -> {plain, shifted}, usage ids 0x04..0x38
static const char keymap[][2] = {
    [HID_KEY_A] = {'a','A'}, [HID_KEY_B] = {'b','B'}, [HID_KEY_C] = {'c','C'},
    [HID_KEY_D] = {'d','D'}, [HID_KEY_E] = {'e','E'}, [HID_KEY_F] = {'f','F'},
    [HID_KEY_G] = {'g','G'}, [HID_KEY_H] = {'h','H'}, [HID_KEY_I] = {'i','I'},
    [HID_KEY_J] = {'j','J'}, [HID_KEY_K] = {'k','K'}, [HID_KEY_L] = {'l','L'},
    [HID_KEY_M] = {'m','M'}, [HID_KEY_N] = {'n','N'}, [HID_KEY_O] = {'o','O'},
    [HID_KEY_P] = {'p','P'}, [HID_KEY_Q] = {'q','Q'}, [HID_KEY_R] = {'r','R'},
    [HID_KEY_S] = {'s','S'}, [HID_KEY_T] = {'t','T'}, [HID_KEY_U] = {'u','U'},
    [HID_KEY_V] = {'v','V'}, [HID_KEY_W] = {'w','W'}, [HID_KEY_X] = {'x','X'},
    [HID_KEY_Y] = {'y','Y'}, [HID_KEY_Z] = {'z','Z'},
    [HID_KEY_1] = {'1','!'}, [HID_KEY_2] = {'2','@'}, [HID_KEY_3] = {'3','#'},
    [HID_KEY_4] = {'4','$'}, [HID_KEY_5] = {'5','%'}, [HID_KEY_6] = {'6','^'},
    [HID_KEY_7] = {'7','&'}, [HID_KEY_8] = {'8','*'}, [HID_KEY_9] = {'9','('},
    [HID_KEY_0] = {'0',')'},
    [HID_KEY_ENTER] = {'\r','\r'}, [HID_KEY_ESC] = {0x1b, 0x1b},
    [HID_KEY_DEL] = {0x7f, 0x7f},  // backspace key
    [HID_KEY_TAB] = {'\t','\t'},  [HID_KEY_SPACE] = {' ',' '},
    [HID_KEY_MINUS] = {'-','_'},  [HID_KEY_EQUAL] = {'=','+'},
    [HID_KEY_OPEN_BRACKET] = {'[','{'}, [HID_KEY_CLOSE_BRACKET] = {']','}'},
    [HID_KEY_BACK_SLASH] = {'\\','|'},
    [HID_KEY_COLON] = {';',':'}, [HID_KEY_QUOTE] = {'\'','"'},
    [HID_KEY_TILDE] = {'`','~'},
    [HID_KEY_LESS] = {',','<'}, [HID_KEY_GREATER] = {'.','>'},
    [HID_KEY_SLASH] = {'/','?'},
};

// Printable ASCII for a usage id (US layout, shift honored) — 0 if none.
// Exposed so ui_home's target editor can route hardware keys into textareas.
char hid_usage_to_ascii(uint8_t usage, uint8_t modifiers)
{
    bool shift = modifiers & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT);
    if (usage >= sizeof(keymap) / sizeof(keymap[0])) return 0;
    char ch = keymap[usage][shift ? 1 : 0];
    return (ch >= 0x20 && ch < 0x7f) ? ch : 0;
}

void hid_send_key(uint8_t usage, uint8_t modifiers)
{
    bool shift = modifiers & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT);
    bool ctrl  = modifiers & (HID_LEFT_CONTROL | HID_RIGHT_CONTROL);
    bool alt   = modifiers & (HID_LEFT_ALT | HID_RIGHT_ALT);

    if (ctrl && alt && usage == HID_KEY_P) {     // panel toggle hotkey
        if (s_hotkey_cb) s_hotkey_cb();
        return;
    }

    // Ctrl+Alt+Up/Down -> PageUp/PageDown: the Tab5 I2C keyboard has no
    // PgUp/PgDn keys, and tmux copy-mode / less / vim all want them.
    if (ctrl && alt && (usage == HID_KEY_UP || usage == HID_KEY_DOWN)) {
        usage = (usage == HID_KEY_UP) ? HID_KEY_PAGEUP : HID_KEY_PAGEDOWN;
        modifiers = 0;
    }

    // Ctrl+Alt+1..9: session tabs — switch to (or lazily open) target N.
    if (ctrl && alt && usage >= HID_KEY_1 && usage <= HID_KEY_9) {
        ssh_client_request_switch(usage - HID_KEY_1);
        return;
    }

    if (ctrl && alt && usage == HID_KEY_W) {     // close the active session
        if (ssh_active_term()) {                 // no-op before SSH starts
            if (ssh_close(ssh_active()) == 0) local_shell_enter();
        }
        return;
    }

    if (ctrl && alt && usage == HID_KEY_T) {     // local shell toggle hotkey
        if (s_shell_cb) s_shell_cb();
        return;
    }

    if (ctrl && alt && usage == HID_KEY_V) {     // voice input start/stop
        voice_input_toggle();
        return;
    }
    if (usage == HID_KEY_ESC && voice_input_is_recording()) {
        voice_input_cancel();                    // discard the take
        return;
    }

    if (power_mgmt_handle_key(usage, modifiers)) return;   // sleep/wake

    // While the full-screen home panel is open it owns the keyboard: Esc
    // closes it, everything else is swallowed (the Ctrl+Alt+* hotkeys above
    // still work, so Ctrl+Alt+P closes too). Touch input is unaffected.
    if (ui_home_is_open()) {
        // The target editor (if open) gets first pick: printable chars go to
        // the focused textarea, Tab/Enter cycle fields, Esc backs out a level.
        if (ui_home_handle_key(usage, modifiers)) return;
        if (usage == HID_KEY_ESC) ui_home_toggle();
        return;
    }

    if (ime_filter_handle_key(usage, modifiers)) return;   // pinyin IME

    char buf[8];
    int n = 0;

    // Keypad encoding follows the terminal the user is typing into: the
    // active session's (DECCKM is per session), or the boot term before SSH.
    term_t *term = ssh_active_term();
    if (!term) term = s_term;

    switch (usage) {
    case HID_KEY_UP:    n = term_encode_key(term, TERM_KEY_UP, buf, sizeof(buf)); break;
    case HID_KEY_DOWN:  n = term_encode_key(term, TERM_KEY_DOWN, buf, sizeof(buf)); break;
    case HID_KEY_RIGHT: n = term_encode_key(term, TERM_KEY_RIGHT, buf, sizeof(buf)); break;
    case HID_KEY_LEFT:  n = term_encode_key(term, TERM_KEY_LEFT, buf, sizeof(buf)); break;
    case HID_KEY_HOME:        n = term_encode_key(term, TERM_KEY_HOME, buf, sizeof(buf)); break;
    case HID_KEY_END:         n = term_encode_key(term, TERM_KEY_END, buf, sizeof(buf)); break;
    case HID_KEY_PAGEUP:      n = term_encode_key(term, TERM_KEY_PGUP, buf, sizeof(buf)); break;
    case HID_KEY_PAGEDOWN:    n = term_encode_key(term, TERM_KEY_PGDN, buf, sizeof(buf)); break;
    case HID_KEY_DELETE:      n = term_encode_key(term, TERM_KEY_DELETE, buf, sizeof(buf)); break;
    default: {
        if (usage >= sizeof(keymap) / sizeof(keymap[0])) return;
        char ch = keymap[usage][shift ? 1 : 0];
        if (!ch) return;
        if (ctrl && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;        // ^A..^Z
        else if (ctrl && ch >= '@' && ch <= '_') ch = ch - '@';
        if (alt) buf[n++] = 0x1b;                                     // ESC prefix
        buf[n++] = ch;
        break;
    }
    }
    if (n > 0) { g_dbg_sink_bytes += n; s_sink((uint8_t *)buf, n); }
}

static void interface_cb(hid_host_device_handle_t dev,
                         const hid_host_interface_event_t event, void *arg)
{
    static uint8_t prev_keys[6];
    uint8_t data[64];
    size_t len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len);
        if (len < sizeof(hid_keyboard_input_report_boot_t)) break;
        hid_keyboard_input_report_boot_t *rpt = (void *)data;
        // emit keys present now but not in the previous report
        for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
            uint8_t k = rpt->key[i];
            if (k <= HID_KEY_ERROR_UNDEFINED) continue;
            bool was_down = false;
            for (int j = 0; j < 6; j++) if (prev_keys[j] == k) was_down = true;
            if (!was_down) hid_send_key(k, rpt->modifier.val);
        }
        memcpy(prev_keys, rpt->key, sizeof(prev_keys));
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "keyboard disconnected");
        if (s_usb_kbd_count > 0) s_usb_kbd_count--;
        hid_host_device_close(dev);
        break;
    default:
        break;
    }
}

static void device_cb(hid_host_device_handle_t dev,
                      const hid_host_driver_event_t event, void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params;
    hid_host_device_get_params(dev, &params);
    if (params.proto != HID_PROTOCOL_KEYBOARD) return;
    s_usb_kbd_count++;

    const hid_host_device_config_t cfg = { .callback = interface_cb };
    hid_host_device_open(dev, &cfg);
    hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
    hid_host_device_start(dev);
    ESP_LOGI(TAG, "keyboard connected");
}

static void usb_events_task(void *arg)
{
    while (true) {
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
    }
}

void hid_keyboard_start(term_t *t)
{
    s_term = t;

    const usb_host_config_t host_cfg = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_events_task, "usb_events", 4096, NULL, 2, NULL);

    const hid_host_driver_config_t drv_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = device_cb,
    };
    ESP_ERROR_CHECK(hid_host_install(&drv_cfg));
    ESP_LOGI(TAG, "USB HID host ready — plug a keyboard into the USB-A port");
}

// Any physical keyboard available? (USB HID currently attached, or the Tab5
// I2C keyboard detected at boot.) Used to skip the on-screen keyboard.
bool hid_keyboard_hw_present(void)
{
    return s_usb_kbd_count > 0 || i2c_keyboard_present();
}

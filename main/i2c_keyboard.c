// Driver for the M5Stack Tab5 Keyboard (I2C, addr 0x6D).
// Protocol (from M5Unit-KEYBOARD library / Tab5 Keyboard datasheet):
//   0x02 EVENT_NUM      queue length 0-32, auto-decrements on event read
//   0x10 MODE_KEYBOARD  0=Normal 1=HID 2=Character
//   0x30 HID_EVENT      2 bytes: modifier + HID keycode; 0xFF 0xFF = empty
//   0xFE FW_VERSION
// HID mode reports the same usage codes as a USB keyboard, so events are
// forwarded to hid_send_key(). Polled every 20ms; INT (G50) is not required.

#include "i2c_keyboard.h"

static volatile bool s_present;
#include "hid_keyboard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "i2c_kbd";

#define KBD_ADDR        0x6D
#define KBD_SDA_GPIO    0
#define KBD_SCL_GPIO    1

#define REG_EVENT_NUM       0x02
#define REG_MODE_KEYBOARD   0x10
#define REG_HID_EVENT       0x30
#define REG_FW_VERSION      0xFE

#define MODE_HID            1
#define POLL_MS             20

static i2c_master_dev_handle_t s_dev;
volatile uint32_t g_dbg_kbd_events;

static bool reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100) == ESP_OK;
}

static bool reg_write(uint8_t reg, uint8_t val)
{
    uint8_t frame[2] = { reg, val };
    return i2c_master_transmit(s_dev, frame, sizeof(frame), 100) == ESP_OK;
}

static void kbd_task(void *arg)
{
    while (true) {
        uint8_t n = 0;
        if (reg_read(REG_EVENT_NUM, &n, 1) && n > 0) {
            if (n > 32) n = 32;
            while (n--) {
                uint8_t ev[2];
                if (!reg_read(REG_HID_EVENT, ev, 2)) break;
                if (ev[0] == 0xFF && ev[1] == 0xFF) break;   // queue empty
                if (ev[1] != 0) { g_dbg_kbd_events++; hid_send_key(ev[1], ev[0]); }  // keycode 0 = modifier only
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void i2c_keyboard_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                  // auto-select
        .sda_io_num = KBD_SDA_GPIO,
        .scl_io_num = KBD_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "Ext.Port1 I2C bus init failed");
        return;
    }

    if (i2c_master_probe(bus, KBD_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no Tab5 Keyboard at 0x%02x (not attached?)", KBD_ADDR);
        i2c_del_master_bus(bus);
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KBD_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    uint8_t fw = 0;
    reg_read(REG_FW_VERSION, &fw, 1);
    if (!reg_write(REG_MODE_KEYBOARD, MODE_HID)) {
        ESP_LOGE(TAG, "failed to set HID mode");
        return;
    }
    ESP_LOGI(TAG, "Tab5 Keyboard ready (fw v%u, HID mode)", fw);
    s_present = true;

    xTaskCreate(kbd_task, "i2c_kbd", 4096, NULL, 5, NULL);
}

bool i2c_keyboard_present(void)
{
    return s_present;
}

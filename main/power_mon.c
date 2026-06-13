// INA226 battery monitor. Probes 0x40..0x44 (manufacturer ID reg must read
// TI's 0x5449), then a low-prio task samples bus/shunt voltage every 2s and
// caches the result for lock-free reads from the UI.

#include "power_mon.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "power_mon";

#define INA226_REG_CONFIG   0x00
#define INA226_REG_SHUNT    0x01    // signed, LSB 2.5uV
#define INA226_REG_BUS      0x02    // LSB 1.25mV
#define INA226_REG_MFG_ID   0xFE    // == 0x5449 ("TI")

// Shunt resistor value in milliohms. M5Stack designs typically use 5 or 10 mΩ;
// adjust here if current readings look off by a clean factor of 2.
#ifndef POWER_MON_SHUNT_MOHM
#define POWER_MON_SHUNT_MOHM 5
#endif

// avg=16, 1.1ms bus+shunt conversion, continuous shunt+bus
#define INA226_CONFIG_VAL   0x4527

static i2c_master_dev_handle_t s_dev;
static bool s_present;

// cached by the poll task, read by the UI
static volatile float s_volts;
static volatile int s_percent;
static volatile bool s_charging;
static volatile bool s_valid;
static volatile float s_current_ma;
static volatile float s_power_mw;

static esp_err_t reg_read16(uint8_t reg, uint16_t *val)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, rx, 2, 100);
    if (err == ESP_OK) *val = ((uint16_t)rx[0] << 8) | rx[1];
    return err;
}

static esp_err_t reg_write16(uint8_t reg, uint16_t val)
{
    uint8_t tx[3] = { reg, val >> 8, val & 0xff };
    return i2c_master_transmit(s_dev, tx, 3, 100);
}

// 2S Li-ion open-circuit-ish voltage curve (6.0-8.4V pack), linear between
// points.
static int batt_percent(float v)
{
    static const struct { float v; int pct; } curve[] = {
        { 8.30f, 100 }, { 7.90f, 80 }, { 7.60f, 60 },
        { 7.40f, 40 },  { 7.10f, 20 }, { 6.80f, 5 },
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (v >= curve[0].v) return 100;
    if (v <= curve[n - 1].v) {
        // below 6.80V: scale 5..0 down to 6.0V
        if (v <= 6.0f) return 0;
        return (int)(curve[n - 1].pct * (v - 6.0f) / (curve[n - 1].v - 6.0f));
    }
    for (int i = 0; i < n - 1; i++) {
        if (v >= curve[i + 1].v) {
            float f = (v - curve[i + 1].v) / (curve[i].v - curve[i + 1].v);
            return curve[i + 1].pct + (int)(f * (curve[i].pct - curve[i + 1].pct) + 0.5f);
        }
    }
    return 0;
}

static void poll_task(void *arg)
{
    while (true) {
        uint16_t bus_raw, shunt_raw;
        if (reg_read16(INA226_REG_BUS, &bus_raw) == ESP_OK
            && reg_read16(INA226_REG_SHUNT, &shunt_raw) == ESP_OK) {
            float v = bus_raw * 0.00125f;               // 1.25mV LSB
            int32_t shunt_uv_x10 = (int16_t)shunt_raw * 25;  // 2.5uV LSB -> 0.1uV units
            s_volts = v;
            // I = Vshunt / Rshunt. shunt_uv_x10 is 0.1uV units:
            // mA = (0.1uV units) / 10 / 1000 / (mΩ/1000) = units / (10 * mΩ)
            float ma = (float)shunt_uv_x10 / (10.0f * POWER_MON_SHUNT_MOHM);
            // The percent table wants the resting voltage, but charge/load
            // current shifts the terminal voltage by I*R (~0.3V at 0.9A) and
            // swings the estimate by ~20 points. Compensate with a rough
            // 350mΩ pack+path resistance; ma > 0 = discharging.
            float v_rest = v + (ma / 1000.0f) * 0.35f;
            s_percent = batt_percent(v_rest);
            s_current_ma = ma;
            s_power_mw = v * ma;
            // Negative shunt voltage = current flowing into the battery.
            // Small deadband so noise around 0 doesn't flicker the icon.
            s_charging = shunt_uv_x10 < -500;           // < -50uV
            s_valid = true;
        } else {
            s_valid = false;
            ESP_LOGW(TAG, "read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool power_mon_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no i2c bus");
        return false;
    }

    for (uint8_t addr = 0x40; addr <= 0x44; addr++) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) continue;
        uint16_t id = 0;
        if (reg_read16(INA226_REG_MFG_ID, &id) == ESP_OK && id == 0x5449) {
            ESP_LOGI(TAG, "INA226 found at 0x%02x", addr);
            s_present = true;
            break;
        }
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    if (!s_present) {
        ESP_LOGW(TAG, "no INA226 on the bus — battery monitor disabled");
        return false;
    }

    if (reg_write16(INA226_REG_CONFIG, INA226_CONFIG_VAL) != ESP_OK) {
        ESP_LOGW(TAG, "config write failed");
    }

    xTaskCreate(poll_task, "power_mon", 3072, NULL, 2, NULL);
    return true;
}

bool power_mon_get(float *volts, int *percent, bool *charging)
{
    if (!s_present || !s_valid) return false;
    if (volts) *volts = s_volts;
    if (percent) *percent = s_percent;
    if (charging) *charging = s_charging;
    return true;
}

bool power_mon_get_ext(float *volts, float *ma, float *mw, bool *charging)
{
    if (!s_present || !s_valid) return false;
    if (volts) *volts = s_volts;
    if (ma) *ma = s_current_ma;
    if (mw) *mw = s_power_mw;
    if (charging) *charging = s_charging;
    return true;
}

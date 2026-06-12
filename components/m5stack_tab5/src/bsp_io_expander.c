/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_err_check.h"
#include "esp_io_expander_pi4ioe5v6408.h"

#include "bsp/m5stack_tab5.h"

static const char *TAG = "Tab5 ioexp";

static esp_io_expander_handle_t io_expander = NULL;  // IO Expander
static esp_io_expander_handle_t io_expander1 = NULL;  // IO Expander

/* PATCHED (tab5_ssh_term): on a cold boot right after flashing, the first I2C
 * transaction to the PI4IOE5V6408 can NACK (chip still powering up at ~1.1s).
 * Retry a few times instead of aborting the whole boot. */
static esp_err_t expander_new_retry(uint8_t addr, esp_io_expander_handle_t *out)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = esp_io_expander_new_i2c_pi4ioe5v6408(bsp_i2c_get_handle(), addr, out);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "expander 0x%02x init failed (attempt %d), retrying", addr, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return err;
}

esp_io_expander_handle_t bsp_io_expander_init(void)
{
    if (io_expander) {
        return io_expander;
    }
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());

    BSP_ERROR_CHECK_RETURN_NULL(expander_new_retry(BSP_IO_EXPANDER_ADDRESS, &io_expander));

    return io_expander;
}

esp_io_expander_handle_t bsp_io_expander1_init(void)
{
    if (io_expander1) {
        return io_expander1;
    }
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());

    BSP_ERROR_CHECK_RETURN_NULL(expander_new_retry(BSP_IO_EXPANDER_ADDRESS_1, &io_expander1));

    return io_expander1;
}

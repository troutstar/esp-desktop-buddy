#include "cyd_touch.h"

#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#define TAG "cyd_touch"

/* XPT2046 on SPI3 (VSPI) */
#define CYD_TOUCH_MOSI 32
#define CYD_TOUCH_MISO 39
#define CYD_TOUCH_CLK  25
#define CYD_TOUCH_CS   33
#define CYD_TOUCH_IRQ  36

/* XPT2046 command bytes */
#define XPT_CMD_X 0xD0
#define XPT_CMD_Y 0x90

/*
 * Raw ADC calibration for CYD landscape orientation.
 * XPT2046 Y-axis maps to screen X; X-axis maps to screen Y.
 * Adjust these if touch registration is off.
 */
#define TOUCH_RAW_MIN 200
#define TOUCH_RAW_MAX 3800

static spi_device_handle_t s_spi;

static uint16_t xpt2046_read(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};
    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_spi, &t);
    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3);
}

esp_err_t cyd_touch_init(void)
{
    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << CYD_TOUCH_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_cfg), TAG, "irq gpio");

    spi_bus_config_t bus = {
        .mosi_io_num = CYD_TOUCH_MOSI,
        .miso_io_num = CYD_TOUCH_MISO,
        .sclk_io_num = CYD_TOUCH_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED), TAG, "bus_init");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CYD_TOUCH_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI3_HOST, &dev, &s_spi), TAG, "add_dev");

    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

bool cyd_touch_read(int *x, int *y)
{
    if (gpio_get_level(CYD_TOUCH_IRQ) != 0) {
        return false;
    }

    /* Average 4 readings for stability */
    uint32_t raw_x = 0, raw_y = 0;
    for (int i = 0; i < 4; i++) {
        raw_x += xpt2046_read(XPT_CMD_X);
        raw_y += xpt2046_read(XPT_CMD_Y);
    }
    raw_x /= 4;
    raw_y /= 4;

    /*
     * In landscape mode (MADCTL 0x28):
     *   XPT Y-axis → screen X
     *   XPT X-axis → screen Y
     */
    int sx = ((int)raw_y - TOUCH_RAW_MIN) * 320 / (TOUCH_RAW_MAX - TOUCH_RAW_MIN);
    int sy = ((int)raw_x - TOUCH_RAW_MIN) * 240 / (TOUCH_RAW_MAX - TOUCH_RAW_MIN);

    sx = sx < 0 ? 0 : (sx > 319 ? 319 : sx);
    sy = sy < 0 ? 0 : (sy > 239 ? 239 : sy);

    *x = sx;
    *y = sy;
    return true;
}

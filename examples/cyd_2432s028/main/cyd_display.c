#include "cyd_display.h"

#include <string.h>
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "cyd_display"

/* Pin assignments — confirmed for ESP32-2432S028 */
#define CYD_PIN_MOSI 13
#define CYD_PIN_MISO 12
#define CYD_PIN_CLK  14
#define CYD_PIN_CS   15
#define CYD_PIN_DC    2
#define CYD_PIN_BL   21

static spi_device_handle_t s_spi;
static uint16_t *s_fb;
static spi_transaction_t s_blit_trans;

int g_cyd_strip_y = 0;

static void IRAM_ATTR s_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(CYD_PIN_DC, (int)(intptr_t)t->user);
}

static void ili9341_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void ili9341_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
        .user = (void *)1,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void ili9341_data1(uint8_t b)
{
    ili9341_data(&b, 1);
}

static void ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t row[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    ili9341_cmd(0x2A);
    ili9341_data(col, 4);
    ili9341_cmd(0x2B);
    ili9341_data(row, 4);
    ili9341_cmd(0x2C);
}

static void ili9341_init_sequence(void)
{
    /* Software reset */
    ili9341_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    ili9341_cmd(0x11); /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Power control A */
    ili9341_cmd(0xCB);
    ili9341_data((uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);

    /* Power control B */
    ili9341_cmd(0xCF);
    ili9341_data((uint8_t[]){0x00, 0xC1, 0x30}, 3);

    /* Driver timing A */
    ili9341_cmd(0xE8);
    ili9341_data((uint8_t[]){0x85, 0x00, 0x78}, 3);

    /* Driver timing B */
    ili9341_cmd(0xEA);
    ili9341_data((uint8_t[]){0x00, 0x00}, 2);

    /* Power on sequence */
    ili9341_cmd(0xED);
    ili9341_data((uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);

    /* Pump ratio */
    ili9341_cmd(0xF7);
    ili9341_data1(0x20);

    /* Power control 1 */
    ili9341_cmd(0xC0);
    ili9341_data1(0x23);

    /* Power control 2 */
    ili9341_cmd(0xC1);
    ili9341_data1(0x10);

    /* VCOM control 1 */
    ili9341_cmd(0xC5);
    ili9341_data((uint8_t[]){0x3E, 0x28}, 2);

    /* VCOM control 2 */
    ili9341_cmd(0xC7);
    ili9341_data1(0x86);

    /* Memory access control: landscape (MY=1, MV=1), BGR panel */
    ili9341_cmd(0x36);
    ili9341_data1(0xA8);

    /* Pixel format: RGB565 */
    ili9341_cmd(0x3A);
    ili9341_data1(0x55);

    /* Frame rate: ~79 Hz */
    ili9341_cmd(0xB1);
    ili9341_data((uint8_t[]){0x00, 0x18}, 2);

    /* Display function control */
    ili9341_cmd(0xB6);
    ili9341_data((uint8_t[]){0x08, 0x82, 0x27}, 3);

    /* 3-gamma disable */
    ili9341_cmd(0xF2);
    ili9341_data1(0x00);

    /* Gamma curve 1 */
    ili9341_cmd(0x26);
    ili9341_data1(0x01);

    /* Positive gamma */
    ili9341_cmd(0xE0);
    ili9341_data((uint8_t[]){0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08,
                              0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15);

    /* Negative gamma */
    ili9341_cmd(0xE1);
    ili9341_data((uint8_t[]){0x00, 0x0E, 0x14, 0x03, 0x11, 0x07,
                              0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15);

    /* Display on */
    ili9341_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t cyd_display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CYD_PIN_DC) | (1ULL << CYD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_set_level(CYD_PIN_BL, 0);
    gpio_set_level(CYD_PIN_DC, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = CYD_PIN_MOSI,
        .miso_io_num = CYD_PIN_MISO,
        .sclk_io_num = CYD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CYD_W * CYD_STRIP_H * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus_init");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CYD_PIN_CS,
        .queue_size = 2,
        .pre_cb = s_pre_cb,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "spi_add_dev");

    s_fb = heap_caps_malloc((size_t)CYD_W * CYD_STRIP_H * sizeof(uint16_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%u bytes)", CYD_W * CYD_STRIP_H * 2u);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, (size_t)CYD_W * CYD_STRIP_H * sizeof(uint16_t));

    ili9341_init_sequence();

    gpio_set_level(CYD_PIN_BL, 1);
    ESP_LOGI(TAG, "display ready, fb=%p", s_fb);
    return ESP_OK;
}

uint16_t *cyd_display_fb(void)
{
    return s_fb;
}

void cyd_display_flush(void)
{
    spi_transaction_t *done;
    int y0 = g_cyd_strip_y;
    int y1 = y0 + CYD_STRIP_H - 1;

    ili9341_set_window(0, y0, CYD_W - 1, y1);

    memset(&s_blit_trans, 0, sizeof(s_blit_trans));
    s_blit_trans.length = (size_t)CYD_W * CYD_STRIP_H * 16;
    s_blit_trans.tx_buffer = s_fb;
    s_blit_trans.user = (void *)1;

    spi_device_queue_trans(s_spi, &s_blit_trans, portMAX_DELAY);
    spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
}

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "picture-ws";

/*-----------------------------------------------------------------------
 * Unihiker K10 LCD wiring (ST7789, 240×240)
 * Adjust these defines if your board revision differs.
 *---------------------------------------------------------------------*/
#define LCD_SPI_HOST     SPI2_HOST
#define PIN_LCD_MOSI     21
#define PIN_LCD_CLK      12
#define PIN_LCD_CS       14
#define PIN_LCD_DC       13
//#define PIN_LCD_RST      14
#define PIN_LCD_BL       21

/*-----------------------------------------------------------------------
 * I2C bus
 *---------------------------------------------------------------------*/
#define I2C_SDA          47
#define I2C_SCL          48
#define I2C_CLK_HZ       (400 * 1000)

static i2c_master_bus_handle_t s_i2c_bus;

/*-----------------------------------------------------------------------
 * XL9535 I2C port extender
 *---------------------------------------------------------------------*/
#define XL9535_ADDR      0x20
#define XL9535_REG_OUT0  0x02   /* Output Port 0 */
#define XL9535_REG_OUT1  0x03   /* Output Port 1 */
#define XL9535_REG_CFG0  0x06   /* Configuration Port 0 (1=input, 0=output) */
#define XL9535_REG_CFG1  0x07   /* Configuration Port 1 */

static i2c_master_dev_handle_t s_xl9535;

#define LCD_H_RES        240
#define LCD_V_RES        240
#define LCD_CLK_HZ       (40 * 1000 * 1000)

/*-----------------------------------------------------------------------
 * PBM image embedded in flash (see EMBED_FILES in CMakeLists.txt)
 *---------------------------------------------------------------------*/
extern const uint8_t pbm_start[] asm("_binary_wind_fox_pbm_start");
extern const uint8_t pbm_end[]   asm("_binary_wind_fox_pbm_end");

static esp_lcd_panel_handle_t s_panel;

/*-----------------------------------------------------------------------
 * RGB888 → RGB565, byte-swapped for big-endian SPI transfer to ST7789
 *---------------------------------------------------------------------*/
static inline uint16_t rgb565_be(uint32_t rgb)
{
    uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800)
                          | ((rgb >> 5) & 0x07E0)
                          | ((rgb >> 3) & 0x001F));
    return __builtin_bswap16(c);
}

/*-----------------------------------------------------------------------
 * Initialise SPI bus + ST7789 panel, turn on backlight
 *---------------------------------------------------------------------*/
static void lcd_init(void)
{
    /* Backlight GPIO */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_LCD_BL, 1);

    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* SPI panel IO */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = LCD_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io));

    /* ST7789 panel driver */
    esp_lcd_panel_dev_config_t dev_cfg = {
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* dir=2 in MicroPython → 180° rotation → mirror both axes */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "LCD initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
}

/*-----------------------------------------------------------------------
 * Initialise I2C master bus
 *---------------------------------------------------------------------*/
static void i2c_init(void)
{
    static i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C initialised (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);
}

/*-----------------------------------------------------------------------
 * Initialise XL9535 port extender: pin 0 → output high
 *---------------------------------------------------------------------*/
static void xl9535_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = XL9535_ADDR,
        .scl_speed_hz    = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9535));

    /* Set pin 0 of port 0 as output (clear bit 0, default 0xFF = all inputs) */
    uint8_t cfg_cmd[] = { XL9535_REG_CFG0, 0xFE };
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, cfg_cmd, sizeof(cfg_cmd), -1));

    /* Drive pin 0 high */
    uint8_t out_cmd[] = { XL9535_REG_OUT0, 0x01 };
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, out_cmd, sizeof(out_cmd), -1));

    ESP_LOGI(TAG, "XL9535 pin 0 → output HIGH");
}

/*-----------------------------------------------------------------------
 * Fill the entire screen with a single RGB888 colour
 * Equivalent to: screen.show_bg(color=...)
 *---------------------------------------------------------------------*/
static void fill_screen(uint32_t colour)
{
    uint16_t c = rgb565_be(colour);

    uint16_t *row = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t),
                                     MALLOC_CAP_DMA);
    assert(row);
    for (int i = 0; i < LCD_H_RES; i++) {
        row[i] = c;
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, row);
    }
    free(row);
}

/*-----------------------------------------------------------------------
 * Parse and draw a binary PBM (P4) image from memory
 * Equivalent to: load_pbm(filename, offset_x, offset_y)
 *---------------------------------------------------------------------*/
static void draw_pbm(const uint8_t *data, size_t size, int ox, int oy)
{
    const uint8_t *p   = data;
    const uint8_t *end = data + size;

    /* --- magic "P4" ------------------------------------------------ */
    if (p + 3 > end || p[0] != 'P' || p[1] != '4') {
        ESP_LOGE(TAG, "Not a valid P4 PBM file");
        return;
    }
    p += 2;
    while (p < end && *p != '\n') p++;
    p++;

    /* --- skip comment lines ---------------------------------------- */
    while (p < end && *p == '#') {
        while (p < end && *p != '\n') p++;
        p++;
    }

    /* --- width and height ------------------------------------------ */
    int w = 0, h = 0;
    while (p < end && *p >= '0' && *p <= '9') { w = w * 10 + (*p++ - '0'); }
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    while (p < end && *p >= '0' && *p <= '9') { h = h * 10 + (*p++ - '0'); }
    while (p < end && *p != '\n') p++;
    p++;                                        /* skip to pixel data */

    ESP_LOGI(TAG, "PBM image: %dx%d", w, h);

    int row_bytes = (w + 7) / 8;
    uint16_t white = rgb565_be(0xFFFFFF);
    uint16_t black = rgb565_be(0x000000);

    uint16_t *line = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(line);

    for (int row = 0; row < h; row++) {
        const uint8_t *src = p + row * row_bytes;
        if (src + row_bytes > end) break;

        for (int col = 0; col < w; col++) {
            int bit = (src[col / 8] >> (7 - col % 8)) & 1;
            line[col] = bit ? black : white;   /* PBM: 1=black, 0=white */
        }
        esp_lcd_panel_draw_bitmap(s_panel,
                                  ox, oy + row,
                                  ox + w, oy + row + 1,
                                  line);
    }
    free(line);
    ESP_LOGI(TAG, "PBM drawn at (%d,%d)", ox, oy);
}

/*-----------------------------------------------------------------------
 * app_main — direct translation of mpy/main.py
 *---------------------------------------------------------------------*/
void app_main(void)
{

    i2c_init();
    xl9535_init();

    lcd_init();
    /* screen.show_bg(color=0xFFFF00) */
    fill_screen(0xFFFF00);

    /* load_pbm('wind-fox.pbm') */
    draw_pbm(pbm_start, pbm_end - pbm_start, 0, 0);

    ESP_LOGI(TAG, "Done");

    /* while True: time.sleep(1) */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
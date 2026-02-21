#include <string.h>
#include <png.h>
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

i2c_master_bus_handle_t s_i2c_bus;

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

static esp_lcd_panel_handle_t s_panel;

/*-----------------------------------------------------------------------
 * PNG image embedded in flash (see EMBED_FILES in CMakeLists.txt)
 *---------------------------------------------------------------------*/
extern const uint8_t png_start[] asm("_binary_wind_fox_a_png_start");
extern const uint8_t png_end[]   asm("_binary_wind_fox_a_png_end");

/*-----------------------------------------------------------------------
 * RGB888 → RGB565, byte-swapped for big-endian SPI transfer to ST7789
 *---------------------------------------------------------------------*/
static uint16_t rgb565(const uint8_t r, const uint8_t g, const uint8_t b)
{
    uint16_t c = (uint16_t)(((b << 8) & 0xF800)
        | ((g << 3) & 0x07E0)
        | ((r >> 3) & 0x001F));
    return c;
}

static uint16_t rgb565_be(const uint8_t r, const uint8_t g, const uint8_t b)
{
    return __builtin_bswap16(rgb565(r, g, b));
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
    esp_lcd_panel_io_handle_t io = nullptr;
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
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
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
static void fill_screen(const uint8_t r, const uint8_t g, const uint8_t b)
{
    uint16_t c = rgb565_be(r, g, b);

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
 * Custom read callback for libpng to read from memory
 *---------------------------------------------------------------------*/
typedef struct {
    const uint8_t *data;
    size_t         offset;
} png_mem_read_t;

static void png_mem_read_fn(png_structp png, png_bytep out, size_t count)
{
    png_mem_read_t *state = (png_mem_read_t *)png_get_io_ptr(png);
    memcpy(out, state->data + state->offset, count);
    state->offset += count;
}

/*-----------------------------------------------------------------------
 * Decode embedded PNG and draw at (ox, oy)
 *---------------------------------------------------------------------*/
static void draw_png(const uint8_t *data, size_t size, int ox, int oy)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                            nullptr, nullptr, nullptr);
    assert(png);
    png_infop info = png_create_info_struct(png);
    assert(info);

    png_mem_read_t state = { .data = data, .offset = 0 };
    png_set_read_fn(png, &state, png_mem_read_fn);
    png_read_info(png, info);

    int w = png_get_image_width(png, info);
    int h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth  = png_get_bit_depth(png, info);

    ESP_LOGI(TAG, "PNG: %dx%d, color_type=%d, bit_depth=%d", w, h,
             color_type, bit_depth);

    /* Normalize to 8-bit RGB */
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    /* Strip alpha — we don't need it for the LCD */
    if (color_type & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);

    png_read_update_info(png, info);

    /* Allocate one row of RGB888 and one row of RGB565 for DMA */
    png_bytep row_rgb = malloc(png_get_rowbytes(png, info));
    uint16_t *row_565 = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(row_rgb && row_565);

    for (int y = 0; y < h; y++) {
        png_read_row(png, row_rgb, nullptr);
        for (int x = 0; x < w; x++) {
            row_565[x] = rgb565_be(row_rgb[x * 3 + 0], row_rgb[x * 3 + 1], row_rgb[x * 3 + 2]);
        }
        esp_lcd_panel_draw_bitmap(s_panel, ox, oy + y, ox + w, oy + y + 1,
                                  row_565);
    }

    free(row_rgb);
    free(row_565);
    png_destroy_read_struct(&png, &info, nullptr);
    ESP_LOGI(TAG, "PNG drawn at (%d,%d)", ox, oy);
}

void sensor_task(void  *);
/*-----------------------------------------------------------------------
 * app_main — direct translation of mpy/main.py
 *---------------------------------------------------------------------*/
_Noreturn void app_main(void)
{

    i2c_init();
    xl9535_init();

    xTaskCreate(sensor_task, "sensor", 4096, nullptr, 5, nullptr);

    lcd_init();
    /* screen.show_bg(color=0xFFFF00) */
    fill_screen(0xFF, 0x7F, 0x00);

    draw_png(png_start, png_end - png_start, 0, 0);

    ESP_LOGI(TAG, "Done");

    /* while True: time.sleep(1) */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
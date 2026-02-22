#include "esp_lcd_io_spi.h"
#include "esp_lcd_types.h"
#include "driver/i2c_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "hw_support.h"

static const char* TAG = "hw";

/*-----------------------------------------------------------------------
 * Unihiker K10 LCD wiring (ILI9341, 240x320)
 *---------------------------------------------------------------------*/
#define LCD_SPI_HOST     SPI2_HOST
#define PIN_LCD_MOSI     21
#define PIN_LCD_CLK      12
#define PIN_LCD_CS       GPIO_NUM_14
#define PIN_LCD_DC       GPIO_NUM_13

/*-----------------------------------------------------------------------
 * I2C bus
 *---------------------------------------------------------------------*/
#define I2C_SDA          GPIO_NUM_47
#define I2C_SCL          GPIO_NUM_48
#define I2C_CLK_HZ       (400 * 1000)

i2c_master_bus_handle_t s_i2c_bus;

/*-----------------------------------------------------------------------
 * XL9535 I2C port extender
 *---------------------------------------------------------------------*/
#define XL9535_ADDR      0x20
#define XL9535_REG_OUT0  0x02
#define XL9535_REG_OUT1  0x03
#define XL9535_REG_CFG0  0x06
#define XL9535_REG_CFG1  0x07

static i2c_master_dev_handle_t s_xl9535;

#define LCD_CLK_HZ       (40 * 1000 * 1000)

esp_lcd_panel_handle_t s_panel;
/*-----------------------------------------------------------------------
 * Initialise SPI bus + ILI9341 panel, turn on backlight
 *---------------------------------------------------------------------*/
static void lcd_init()
{

    constexpr spi_bus_config_t bus = {
        .mosi_io_num   = PIN_LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t dev_cfg = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .reset_gpio_num = GPIO_NUM_NC,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &dev_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* dir=2 in MicroPython -> 180 deg rotation -> mirror both axes */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));

    ESP_LOGI(TAG, "LCD initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
}

/*------------------------------------------------------------------------
 * Initialise I2C master bus
 *---------------------------------------------------------------------*/
static void i2c_init()
{
    static i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true,.allow_pd = 0},
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C initialised (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);
}

/*-----------------------------------------------------------------------
 * Initialise XL9535 port extender: pin 0 -> output high
 *---------------------------------------------------------------------*/
static void xl9535_init()
{
    constexpr  i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9535_ADDR,
        .scl_speed_hz = I2C_CLK_HZ,
        .scl_wait_us = 0,
        .flags = {},
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9535));

    uint8_t cfg_cmd[] = {XL9535_REG_CFG0, 0xFE};
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, cfg_cmd, sizeof(cfg_cmd), -1));
}

void hw_init()
{
    i2c_init();
    xl9535_init();
    lcd_init();
}

void backlight(bool on)
{
    uint8_t out_cmd[] = {XL9535_REG_OUT0, on ? 0x01 : 0};
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, out_cmd, sizeof(out_cmd), -1));

    ESP_LOGI(TAG, "XL9535 pin 0 -> output HIGH");

}
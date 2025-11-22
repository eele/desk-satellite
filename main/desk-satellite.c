#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"

// 引入 ST7735 驱动头文件 (需先 add-dependency)
#include "esp_lcd_st7735.h"

static const char *TAG = "ST7735_DEMO";

// --- 引脚定义 ---
#define LCD_HOST    SPI2_HOST
#define PIN_NUM_CLK  0
#define PIN_NUM_MOSI 1
#define PIN_NUM_RST  2
#define PIN_NUM_DC   3
#define PIN_NUM_CS   4
#define PIN_NUM_BCKL 5

// --- 屏幕参数 (0.96寸通常是 80x160，但也可能偏移) ---
// --- 屏幕参数 (横向 Landscape) ---
#define LCD_H_RES   160  // 宽变长
#define LCD_V_RES   80   // 高变短
#define LCD_OFFSET_X 26 // ST7735 0.96寸常见的偏移量，如果显示偏了请调整这里
#define LCD_OFFSET_Y 1

// --- 极简 5x7 字体数据 (仅包含 Hello World 所需字符以节省篇幅) ---
// 实际项目中建议使用完整的 font.h
const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space (index 0)
    {0x00, 0x00, 0x5f, 0x00, 0x00}, // !
    {0x07, 0x00, 0x07, 0x00, 0x00}, // "
    // ... 为了代码简洁，这里通过 ASCII 偏移映射简单的子集 ...
    // 这里使用程序化生成的字模只是为了演示。
};

// 标准 ASCII 5x7 字模片段 (H, e, l, o, W, r, d, ,)
// 格式: 5 bytes per char.
const uint8_t simple_font_map[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // Space
    0x00, 0x00, 0x5F, 0x00, 0x00, // !
    0x00, 0x60, 0x60, 0x00, 0x00, // ,
    0x7F, 0x08, 0x08, 0x08, 0x7F, // H
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x00, 0x41, 0x7F, 0x40, 0x00, // l
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x3F, 0x40, 0x38, 0x40, 0x3F, // W
    0x7C, 0x08, 0x04, 0x04, 0x08, // r
    0x38, 0x44, 0x44, 0x48, 0x7F, // d
};

// 颜色定义 (RGB565)
#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800

// --- 辅助函数：在指定位置画一个字符 ---
// 注意：这是一个低效率的实现（逐像素），仅用于演示
void lcd_draw_char(esp_lcd_panel_handle_t panel_handle, uint16_t x, uint16_t y, char c, uint16_t color) {
    uint8_t char_index = 0;
    // 简单的字符映射逻辑 (仅针对上面的 simple_font_map)
    if (c == ' ') char_index = 0;
    else if (c == '!') char_index = 1;
    else if (c == ',') char_index = 2;
    else if (c == 'H') char_index = 3;
    else if (c == 'e') char_index = 4;
    else if (c == 'l') char_index = 5;
    else if (c == 'o') char_index = 6;
    else if (c == 'W') char_index = 7;
    else if (c == 'r') char_index = 8;
    else if (c == 'd') char_index = 9;
    else return; // 未知字符

    const uint8_t *bitmap = &simple_font_map[char_index * 5];
    uint16_t buffer[1];

    for (int i = 0; i < 5; i++) { // 5 columns
        uint8_t line = bitmap[i];
        for (int j = 0; j < 7; j++) { // 7 rows
            if (line & 0x01) {
                buffer[0] = (color >> 8) | (color << 8); // SPI 传输通常需要字节交换
                esp_lcd_panel_draw_bitmap(panel_handle, x + i, y + j, x + i + 1, y + j + 1, buffer);
            }
            line >>= 1;
        }
    }
}

// --- 辅助函数：显示字符串 ---
void lcd_draw_string(esp_lcd_panel_handle_t panel_handle, uint16_t x, uint16_t y, const char *text, uint16_t color) {
    while (*text) {
        lcd_draw_char(panel_handle, x, y, *text, color);
        x += 6; // 字符宽度5 + 间距1
        text++;
    }
}

void app_main(void)
{
    // 1. 初始化背光
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_BCKL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_NUM_BCKL, 1); // 开启背光

    // 2. 初始化 SPI 总线
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2 + 8
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 3. 安装 LCD Panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 20 * 1000 * 1000, // 20MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // 4. 安装 ST7735 Panel 驱动
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB, // 根据屏幕可能需要调整为 BGR
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));

    // 5. 配置屏幕参数
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // --- 横向显示配置 (Landscape) ---

    // 1. 交换 X 和 Y 轴 (这是横屏的关键)
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));

    // 2. 镜像设置 (根据屏幕实际贴装方向，可能需要调整这两个 true/false)
    //通常横屏需要 Mirror Y 为 true，Mirror X 为 false
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));

    // 3. 设置偏移量 (Gap)
    // ST7735 0.96寸横屏时的偏移量通常需要互换并微调
    // 如果你发现图像上下偏了，调第2个参数；左右偏了，调第1个参数
    // 常见组合：(1, 26) 或 (0, 24) 或 (26, 1)
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 1, 26));

    // 4. 颜色反转 (保持原样)
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Display Initialized");

    // 6. 清屏 (填充黑色)
    // 分配一个全屏 buffer 会消耗较多内存，这里我们分块清屏或逐像素画
    // 为了简单，我们画一个黑色矩形覆盖全屏 (假设 80x160)
    uint16_t *color_buf = heap_caps_malloc(LCD_H_RES * 10 * sizeof(uint16_t), MALLOC_CAP_DMA);
    memset(color_buf, 0, LCD_H_RES * 10 * sizeof(uint16_t)); // Black
    for (int y = 0; y < LCD_V_RES; y += 10) {
         esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 10, color_buf);
    }
    free(color_buf);

    // 7. 显示 Hello World (坐标调整)
    ESP_LOGI(TAG, "Drawing Text Landscape");

    // 注意 Y 轴现在最大只有 80
    lcd_draw_string(panel_handle, 10, 20, "Hello, World!", RED);
    lcd_draw_string(panel_handle, 10, 40, "World, Hello!", BLUE);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
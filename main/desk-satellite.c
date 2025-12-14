#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- 1. 引脚定义 (基于你的要求) ---
#define PIN_NUM_CLK  0
#define PIN_NUM_MOSI 1
#define PIN_NUM_RST  2
#define PIN_NUM_DC   3
#define PIN_NUM_CS   4
#define PIN_NUM_BCKL 5

// --- 2. 屏幕参数 & 颜色定义 ---
// 0.96寸 ST7735 通常物理分辨率为 80x160，但在驱动中显存是 132x162
// 横屏模式下，我们需要处理偏移量
#define LCD_WIDTH  160
#define LCD_HEIGHT 80
#define LCD_OFFSET_X 0 // 偏移量取决于具体的屏幕批次，如果画面偏了，调整这个
#define LCD_OFFSET_Y 23

// 颜色定义 (RGB565格式)
#define COLOR_BLACK     0x0000
#define COLOR_LIGHT_BLUE 0x4DFF // 淡蓝色 (类似于图中的颜色)

static const char *TAG = "ROBOT_EYE";
spi_device_handle_t spi;

// --- 3. SPI 底层传输函数 ---

// 发送指令
void lcd_cmd(const uint8_t cmd) {
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void*)0; // D/C 引脚设为 0 (Command)

    gpio_set_level(PIN_NUM_DC, 0);
    ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

// 发送数据
void lcd_data(const uint8_t *data, int len) {
    if (len == 0) return;
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * len;
    t.tx_buffer = data;
    t.user = (void*)1; // D/C 引脚设为 1 (Data)

    gpio_set_level(PIN_NUM_DC, 1);
    ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

// 发送单个字节数据
void lcd_data_byte(const uint8_t data) {
    lcd_data(&data, 1);
}

// --- 4. ST7735 初始化与设置 ---

void lcd_init() {
    // 硬件复位
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // 软件复位
    lcd_cmd(0x01); // SWRESET
    vTaskDelay(150 / portTICK_PERIOD_MS);

    // 退出睡眠
    lcd_cmd(0x11); // SLPOUT
    vTaskDelay(255 / portTICK_PERIOD_MS);

    // 颜色模式: 16-bit (RGB565)
    lcd_cmd(0x3A); // COLMOD
    lcd_data_byte(0x05);

    // 显存数据访问控制 (方向设置)
    // 0xA0 = Y-X 交换 (横屏) + BGR颜色顺序(大部分ST7735是BGR)
    // 如果颜色红蓝反了，修改这里。如果屏幕倒了，修改这里。
    lcd_cmd(0x36); // MADCTL
    lcd_data_byte(0xA8); // MX, MY, MV, RGB 组合，适配横屏

    // 显示反转 (IPS屏幕通常需要开启反转，如果黑色变白色，请注释掉这行)
    lcd_cmd(0x21); // INVON

    // 打开显示
    lcd_cmd(0x29); // DISPON
}

// 设置绘制窗口
void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    // 加上偏移量
    x1 += LCD_OFFSET_X; x2 += LCD_OFFSET_X;
    y1 += LCD_OFFSET_Y; y2 += LCD_OFFSET_Y;

    uint8_t data[4];

    // 列地址设置
    lcd_cmd(0x2A); // CASET
    data[0] = x1 >> 8; data[1] = x1 & 0xFF;
    data[2] = x2 >> 8; data[3] = x2 & 0xFF;
    lcd_data(data, 4);

    // 行地址设置
    lcd_cmd(0x2B); // RASET
    data[0] = y1 >> 8; data[1] = y1 & 0xFF;
    data[2] = y2 >> 8; data[3] = y2 & 0xFF;
    lcd_data(data, 4);

    lcd_cmd(0x2C); // RAMWR
}

// 填充矩形区域
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if((x >= LCD_WIDTH) || (y >= LCD_HEIGHT)) return;
    if((x + w - 1) >= LCD_WIDTH)  w = LCD_WIDTH  - x;
    if((y + h - 1) >= LCD_HEIGHT) h = LCD_HEIGHT - y;

    lcd_set_window(x, y, x + w - 1, y + h - 1);

    // 创建一行的数据缓冲区来加速SPI传输
    uint8_t *line_buffer = heap_caps_malloc(w * 2, MALLOC_CAP_DMA);
    if (!line_buffer) return;

    for (int i = 0; i < w; i++) {
        line_buffer[i*2] = color >> 8;
        line_buffer[i*2+1] = color & 0xFF;
    }

    // 发送 h 次行数据
    for (int i = 0; i < h; i++) {
        lcd_data(line_buffer, w * 2);
    }

    free(line_buffer);
}

// 绘制单个像素
void lcd_draw_pixel(int x, int y, uint16_t color) {
    if ((x < 0) || (x >= LCD_WIDTH) || (y < 0) || (y >= LCD_HEIGHT)) return;
    lcd_set_window(x, y, x, y);
    uint8_t data[2] = {color >> 8, color & 0xFF};
    lcd_data(data, 2);
}

// --- 5. 图形绘制算法：圆角矩形 ---

// 辅助：绘制圆角部分 (Bresenham算法变种)
void draw_circle_helper(int16_t x0, int16_t y0, int16_t r, uint8_t cornername, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        if (cornername & 0x1) { // 右下角
            lcd_draw_pixel(x0 + x, y0 + y, color);
            lcd_draw_pixel(x0 + y, y0 + x, color);
        }
        if (cornername & 0x2) { // 右上角
            lcd_draw_pixel(x0 + x, y0 - y, color);
            lcd_draw_pixel(x0 + y, y0 - x, color);
        }
        if (cornername & 0x4) { // 左下角
            lcd_draw_pixel(x0 - y, y0 + x, color);
            lcd_draw_pixel(x0 - x, y0 + y, color);
        }
        if (cornername & 0x8) { // 左上角
            lcd_draw_pixel(x0 - y, y0 - x, color);
            lcd_draw_pixel(x0 - x, y0 - y, color);
        }
    }
}

// 辅助：填充圆角部分
void fill_circle_helper(int16_t x0, int16_t y0, int16_t r, uint8_t cornername, int16_t delta, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        if (cornername & 0x1) { // Right segments
            lcd_fill_rect(x0 + x, y0 - y, 1, 2 * y + 1 + delta, color);
            lcd_fill_rect(x0 + y, y0 - x, 1, 2 * x + 1 + delta, color);
        }
        if (cornername & 0x2) { // Left segments
            lcd_fill_rect(x0 - x, y0 - y, 1, 2 * y + 1 + delta, color);
            lcd_fill_rect(x0 - y, y0 - x, 1, 2 * x + 1 + delta, color);
        }
    }
}

// 绘制填充的圆角矩形
void draw_filled_rounded_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    // 绘制中间的主矩形
    lcd_fill_rect(x + r, y, w - 2 * r, h, color);

    // 绘制左边和右边的矩形部分（除了圆角剩下的部分）
    // 为了简化，这里我们使用 fill_circle_helper 来处理四个角和侧边填充
    // 但更简单的方法是：绘制三个矩形 + 四个角的圆弧填充

    // 方案B：简单几何拼接
    // 1. 中间竖条
    // lcd_fill_rect(x + r, y, w - 2 * r, h, color); // 上面已画
    // 2. 左侧竖条（不含角）
    lcd_fill_rect(x, y + r, r, h - 2 * r, color);
    // 3. 右侧竖条（不含角）
    lcd_fill_rect(x + w - r, y + r, r, h - 2 * r, color);

    // 4. 填充四个圆角
    // 左上
    fill_circle_helper(x + r, y + r, r, 0, 0, color);
    // 这里 fill_circle_helper 比较通用，我们手动画四个角的扇形填充比较繁琐，
    // 最简单的 Hack 是画实心圆然后覆盖，但效率低。
    // 我们用一种简单的逐行扫描圆的方法来补全四个角：

    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t cx = 0;
    int16_t cy = r;

    while (cx < cy) {
        if (f >= 0) {
            cy--;
            ddF_y += 2;
            f += ddF_y;
        }
        cx++;
        ddF_x += 2;
        f += ddF_x;

        // 绘制四个角的水平线
        // 左上角
        lcd_fill_rect(x + r - cy, y + r - cx, cy - cx, 1, color); // 补缝隙
        lcd_fill_rect(x + r - cx, y + r - cy, cx, 1, color);
        lcd_draw_pixel(x + r - cy, y + r - cx, color); // 边缘点
        lcd_draw_pixel(x + r - cx, y + r - cy, color);

        // 右上角
        lcd_fill_rect(x + w - r + cx, y + r - cy, cy - cx, 1, color);
        lcd_draw_pixel(x + w - r + cy, y + r - cx, color);
        // ... 这个算法在嵌入式手写有点复杂，我们用一个更笨但稳定的方法：
        // 重新定义一个简单的填充圆角函数，利用对称性画水平线
    }
}

// 简化的实心圆角矩形绘制函数 (优化的扫描线法)
void draw_robot_eye(int x, int y, int w, int h, int r, uint16_t color) {
    // 限制圆角半径
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;

    // 1. 绘制中间的主体矩形 (高度为 h)
    lcd_fill_rect(x + r, y, w - 2 * r, h, color);

    // 2. 绘制左右两侧的矩形 (高度为 h - 2r)
    lcd_fill_rect(x, y + r, r, h - 2 * r, color);
    lcd_fill_rect(x + w - r, y + r, r, h - 2 * r, color);

    // 3. 填充四个角 (Bresenham)
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int cx = 0;
    int cy = r;

    while (cx < cy) {
        if (f >= 0) {
            cy--;
            ddF_y += 2;
            f += ddF_y;
        }
        cx++;
        ddF_x += 2;
        f += ddF_x;

        // 上方角 (Top)
        lcd_fill_rect(x + r - cx, y + r - cy, cx, 1, color); // 左上 part 1
        lcd_fill_rect(x + r - cy, y + r - cx, cy, 1, color); // 左上 part 2

        lcd_fill_rect(x + w - r, y + r - cy, cx, 1, color);  // 右上 part 1
        lcd_fill_rect(x + w - r, y + r - cx, cy, 1, color);  // 右上 part 2

        // 下方角 (Bottom)
        lcd_fill_rect(x + r - cx, y + h - r + cy, cx, 1, color); // 左下 part 1
        lcd_fill_rect(x + r - cy, y + h - r + cx, cy, 1, color); // 左下 part 2

        lcd_fill_rect(x + w - r, y + h - r + cy, cx, 1, color); // 右下 part 1
        lcd_fill_rect(x + w - r, y + h - r + cx, cy, 1, color); // 右下 part 2
    }
}

// --- 6. 主程序 ---

void app_main(void) {
    esp_err_t ret;

    // 1. 配置 GPIO
    gpio_reset_pin(PIN_NUM_DC);
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_reset_pin(PIN_NUM_RST);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_reset_pin(PIN_NUM_BCKL);
    gpio_set_direction(PIN_NUM_BCKL, GPIO_MODE_OUTPUT);

    // 2. 配置 SPI 总线
    spi_bus_config_t buscfg = {
        .miso_io_num = -1, // 不使用 MISO
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2 + 8
    };

    // 3. 配置 SPI 设备
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000, // 20 MHz
        .mode = 0,                          // SPI mode 0
        .spics_io_num = PIN_NUM_CS,         // CS 引脚
        .queue_size = 7,
    };

    // 初始化 SPI
    // 注意：ESP32 的 SPI 主机选择 SPI2_HOST 或 SPI3_HOST
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    // 4. 初始化屏幕
    ESP_LOGI(TAG, "Initializing LCD...");
    lcd_init();

    // 5. 打开背光
    gpio_set_level(PIN_NUM_BCKL, 1);

    // 6. 绘制内容
    ESP_LOGI(TAG, "Drawing Robot Eyes...");

    // 清屏黑色
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK);

    // 定义眼睛参数 (基于 160x80 分辨率)
    int eye_width = 45;
    int eye_height = 40;
    int eye_radius = 9; // 圆角半径
    int gap = 10;        // 两眼间距
    int start_y = (LCD_HEIGHT - eye_height) / 2; // 垂直居中

    // 计算 X 坐标
    int total_width = (eye_width * 2) + gap;
    int start_x_left = (LCD_WIDTH - total_width) / 2;
    int start_x_right = start_x_left + eye_width + gap;

    // 绘制左眼
    draw_robot_eye(start_x_left, start_y, eye_width, eye_height, eye_radius, COLOR_LIGHT_BLUE);

    // 绘制右眼
    draw_robot_eye(start_x_right, start_y, eye_width, eye_height, eye_radius, COLOR_LIGHT_BLUE);

    ESP_LOGI(TAG, "Done.");

    // --- 5. 动画主循环 (修改部分) ---
    ESP_LOGI(TAG, "Starting blink animation loop...");
    while (1) {
        // 1. 睁眼持续时间 (随机)
        // 持续 1 到 5 秒 (1000ms - 5000ms)
        int open_duration = (rand() % 4000) + 1000;
        vTaskDelay(open_duration / portTICK_PERIOD_MS);

        // 2. 闭眼：用黑色背景色覆盖眼睛
        // 左眼闭合
        lcd_fill_rect(start_x_left, start_y, eye_width, eye_height, COLOR_BLACK);
        // 右眼闭合
        lcd_fill_rect(start_x_right, start_y, eye_width, eye_height, COLOR_BLACK);

        ESP_LOGI(TAG, "Eyes closed...");

        // 3. 闭眼持续时间
        // 持续 50ms - 200ms (快速眨眼)
        int blink_duration = (rand() % 150) + 50;
        vTaskDelay(blink_duration / portTICK_PERIOD_MS);

        // 4. 重新睁眼：重新绘制淡蓝色圆角矩形
        draw_robot_eye(start_x_left, start_y, eye_width, eye_height, eye_radius, COLOR_LIGHT_BLUE);
        draw_robot_eye(start_x_right, start_y, eye_width, eye_height, eye_radius, COLOR_LIGHT_BLUE);

        ESP_LOGI(TAG, "Eyes opened...");
    }
}
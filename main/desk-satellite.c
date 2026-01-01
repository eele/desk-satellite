#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "SPEECH_DRIVER";

/**
 * ============================================================
 * 配置部分
 * ============================================================
 */
#define I2C_MASTER_SCL_IO           20      /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           21      /*!< GPIO number used for I2C master data */
#define I2C_MASTER_NUM              0       /*!< I2C master i2c port number */
#define I2C_MASTER_FREQ_HZ          100000  /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0       /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0       /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000

#define SPEECH_ADDR                 0x30    // 语音识别模块地址
#define DATA_HEAD                   0xFD

// 芯片状态定义
#define CHIP_STATUS_INIT_SUCCESS    0x4A
#define CHIP_STATUS_CORRECT_CMD     0x41
#define CHIP_STATUS_ERROR_CMD       0x45
#define CHIP_STATUS_BUSY            0x4E
#define CHIP_STATUS_IDLE            0x4F

// 编码格式
#define ENCODING_GB2312             0x00
#define ENCODING_GBK                0x01
#define ENCODING_BIG5               0x02
#define ENCODING_UNICODE            0x03

// 发音人 ID (对应 Python Reader_Type)
#define READER_XIAOYAN              3
#define READER_XUJIU                51
#define READER_XUDUO                52
#define READER_XIAOPING             53
#define READER_DONALDDUCK           54
#define READER_XUXIAOBAO            55

/**
 * ============================================================
 * I2C 底层驱动函数
 * ============================================================
 */

static esp_err_t i2c_master_init(void)
{
    int i2c_master_port = I2C_MASTER_NUM;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    i2c_param_config(i2c_master_port, &conf);
    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/**
 * ============================================================
 * 语音模块控制函数
 * ============================================================
 */

esp_err_t i2c_send_byte(const uint8_t *data, size_t write_size) {
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < write_size;i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        // 发送设备地址（7位地址 + 写标志）
        i2c_master_write_byte(cmd, (SPEECH_ADDR << 1) | I2C_MASTER_WRITE, true);
        // ESP_LOGI(TAG, "发送数据 0x%02X 到地址 0x%02X", data[i], SPEECH_ADDR);
        i2c_master_write_byte(cmd, data[i], false);
        // 停止信号
        i2c_master_stop(cmd);
        // 执行命令
        ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);

        if (ret == ESP_OK) {
            // ESP_LOGI(TAG, "成功发送数据 0x%02X 到地址 0x%02X", data[i], SPEECH_ADDR);
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2C 超时");
        } else {
            ESP_LOGE(TAG, "发送失败: %s", esp_err_to_name(ret));
        }
        // 释放命令链接
        i2c_cmd_link_delete(cmd);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ret;
}

esp_err_t i2c_read_byte(uint8_t *data, size_t rx_len) {
    esp_err_t ret = ESP_FAIL;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    // 发送设备地址（读模式）
    i2c_master_write_byte(cmd, (SPEECH_ADDR << 1) | I2C_MASTER_READ, true);

    if (rx_len > 1) {
        // 读取多个字节
        i2c_master_read(cmd, data, rx_len, I2C_MASTER_ACK);
    }
    // 读取最后一个字节，发送NACK表示结束
    i2c_master_read_byte(cmd, data + rx_len, I2C_MASTER_NACK);
    // 停止信号
    i2c_master_stop(cmd);
    // 执行命令
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);

    // 释放命令链接
    i2c_cmd_link_delete(cmd);
    // vTaskDelay(10 / portTICK_PERIOD_MS);
    return ret;
}

// 获取芯片状态
uint8_t get_chip_status(void)
{
    uint8_t cmd[4] = {0xFD, 0x00, 0x01, 0x21}; // AskState 命令
    esp_err_t err = i2c_send_byte(cmd, sizeof(cmd));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write Status Cmd Error");
        return 0x00;
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // Python 中的 time.sleep(0.05)

    uint8_t status = 0;
    err = i2c_read_byte(&status, 2);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read Status Error");
        return 0x00;
    }

    // printf("ret 0x%02X\n", status);
    return status;
}

// 等待芯片空闲
void wait_for_idle(void)
{
    // 简单超时保护，防止死循环
    int timeout_counter = 0;
    while (get_chip_status() != CHIP_STATUS_IDLE) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Python loop sleep 0.1s
        timeout_counter++;
        if(timeout_counter > 200) { // 20秒超时
             ESP_LOGW(TAG, "Wait for idle timeout");
             break;
        }
    }
}

// 发送数据包核心函数 (对应 Python 的 I2C_WriteBytes + 协议封装)
// is_command: 1=发送设置命令(编码0x00), 0=发送文本(需指定编码)
void speech_send_packet(const uint8_t *data, size_t len, uint8_t encoding_format)
{
    // 协议结构: [Head, Len_H, Len_L, Cmd, Encoding] + [Data Bytes]
    // Size = 数据长度 + 2 (协议规定)
    size_t size = len + 2;

    uint8_t header[5];
    header[0] = DATA_HEAD;
    header[1] = (uint8_t)(size >> 8);      // Length HH
    header[2] = (uint8_t)(size & 0x00FF);  // Length LL
    header[3] = 0x01;                      // Command
    header[4] = encoding_format;

    // ESP32 I2C 是一次性写入 buffer，不像 Python 那样 byte-by-byte sleep
    // 为了稳定性，我们先发头，再发数据，或者拼接后发送。
    // 这里采用分两次发送，模拟 Python 逻辑

    // 1. 发送帧头
    i2c_send_byte(header, 5);

    // 2. 发送内容
    i2c_send_byte(data, len);
}

// 对应 Python 的 SetBase 和 TextCtrl
// cmd_char: 例如 'v', 'm'
// num: 数值, 如果是 -1 则不带数字
void text_ctrl(char cmd_char, int num)
{
    char buf[16];
    if (num != -1) {
        printf("0x%02X 0x%02X\n", cmd_char, num);
        snprintf(buf, sizeof(buf), "[%c%d]", cmd_char, num);
    } else {
        printf("0x%02X\n", cmd_char);
        snprintf(buf, sizeof(buf), "[%c]", cmd_char);
    }
    // 控制命令一般使用 GB2312 (0x00) 即可
    speech_send_packet((uint8_t*)buf, strlen(buf), ENCODING_GB2312);

    // 每个设置指令后等待空闲，根据 Python 逻辑
    // Python: while GetChipStatus() != ChipStatus_Idle
    // 注意：有些简单指令可能很快，这里为了保险加上短暂延时，然后轮询
    vTaskDelay(pdMS_TO_TICKS(2));
    wait_for_idle();
}

// 对应 Python: SetReader
void set_reader(int num) {
    text_ctrl('m', num);
}

// 对应 Python: SetVolume
void set_volume(int volume) {
    text_ctrl('v', volume);
}

// 对应 Python: Speech_text
void speech_text(const uint8_t *text_data, size_t len, uint8_t encoding) {
    speech_send_packet(text_data, len, encoding);
}

/**
 * ============================================================
 * 主程序逻辑
 * ============================================================
 */
void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C Initialized on SDA: %d, SCL: %d", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    // 给一点启动时间
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Setting Reader to READER_XIAOYAN");
    set_reader(READER_XIAOYAN);

    ESP_LOGI(TAG, "Setting Volume to 10");
    set_volume(6);

    // "你好亚博智能科技" 的 GB2312 编码
    // 你好: C4 E3 BA C3
    // 亚博: D1 C7 B2 A9
    // 智能: D6 C7 C4 DC
    // 科技: BF C6 BC BC
    const uint8_t text1_gb2312[] = {
        0xC4, 0xE3, 0xBA, 0xC3,
        0xD1, 0xC7, 0xB2, 0xA9,
        0xD6, 0xC7, 0xC4, 0xDC,
        0xBF, 0xC6, 0xBC, 0xBC
    };

    // "欢迎使用亚博智能语音播报模块" 的 GB2312 编码
    // 欢迎: BB B6 D3 AD
    // 使用: CA B9 D3 C3
    // 亚博: D1 C7 B2 A9
    // 智能: D6 C7 C4 DC
    // 语音: D3 EF D2 F4
    // 播报: B2 A5 B1 A8
    // 模块: C4 A3 BF E9
    const uint8_t text2_gb2312[] = {
        0xBB, 0xB6, 0xD3, 0xAD,
        0xCA, 0xB9, 0xD3, 0xC3,
        0xD1, 0xC7, 0xB2, 0xA9,
        0xD6, 0xC7, 0xC4, 0xDC,
        0xD3, 0xEF, 0xD2, 0xF4,
        0xB2, 0xA5, 0xB1, 0xA8,
        0xC4, 0xA3, 0xBF, 0xE9
    };

    while (1) {

        ESP_LOGI(TAG, "Speaking Text 1...");
        speech_text(text1_gb2312, sizeof(text1_gb2312), ENCODING_GB2312);

        // 等待播报结束
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_for_idle();

        ESP_LOGI(TAG, "Speaking Text 2...");
        speech_text(text2_gb2312, sizeof(text2_gb2312), ENCODING_GB2312);

        // 等待播报结束
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_for_idle();

        ESP_LOGI(TAG, "Done. Looping...");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
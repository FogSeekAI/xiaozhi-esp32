#include "uart_transport.h"
#include <esp_log.h>
#include <cstring>

#define TAG "UartTransport"

// 协议格式定义：
// [Header: 0xAA 0x55] [Type: 1 byte] [Length: 2 bytes] [Payload: N bytes] [CRC8: 1 byte]
#define PROTOCOL_HEADER_1 0xAA
#define PROTOCOL_HEADER_2 0x55
#define PROTOCOL_OVERHEAD 6 // Header(2) + Type(1) + Length(2) + CRC(1)

UartTransport::UartTransport()
    : uart_port_(UART_NUM_0), tx_pin_(-1), rx_pin_(-1), baud_rate_(0), initialized_(false) {}

UartTransport::~UartTransport()
{
    if (initialized_)
    {
        uart_driver_delete(uart_port_);
        initialized_ = false;
    }
}

bool UartTransport::Initialize(uart_port_t uart_port, int tx_pin, int rx_pin, uint32_t baud_rate)
{
    if (initialized_)
    {
        ESP_LOGW(TAG, "UART already initialized");
        return false;
    }

    uart_port_ = uart_port;
    tx_pin_ = tx_pin;
    rx_pin_ = rx_pin;
    baud_rate_ = baud_rate;

    // UART配置参数
    uart_config_t uart_config = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 配置 UART参数
    esp_err_t err = uart_param_config(uart_port, &uart_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(err));
        return false;
    }

    // 设置引脚
    err = uart_set_pin(uart_port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        return false;
    }

    // 安装 UART驱动 (这会自动处理底层初始化)
    err = uart_driver_install(uart_port, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "UART initialized on port %d, TX=%d, RX=%d, baud=%lu",
             uart_port, tx_pin, rx_pin, baud_rate);
    return true;
}

uint8_t UartTransport::CalculateCRC8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80)
            {
                crc = (crc << 1) ^ 0x07;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

bool UartTransport::SendChatMessage(role_type_t role, const std::string &content)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 构建消息体：{"role": X, "content": "..."}
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"role\":%d,\"content\":\"%s\"}", role, content.c_str());

    size_t payload_len = strlen(payload);
    uint8_t buffer[PROTOCOL_OVERHEAD + payload_len];

    // 填充帧头
    buffer[0] = PROTOCOL_HEADER_1;
    buffer[1] = PROTOCOL_HEADER_2;
    // 消息类型
    buffer[2] = MSG_TYPE_CHAT_MESSAGE;
    // 数据长度（小端序）
    buffer[3] = payload_len & 0xFF;
    buffer[4] = (payload_len >> 8) & 0xFF;
    // 复制 payload
    memcpy(buffer + 5, payload, payload_len);
    // 计算 CRC8
    buffer[5 + payload_len] = CalculateCRC8(buffer, 5 + payload_len);

    ESP_LOGD(TAG, "Sending chat message: role=%d, content=%s", role, content.c_str());
    int sent = uart_write_bytes(uart_port_, buffer, sizeof(buffer));
    return sent == sizeof(buffer);
}

bool UartTransport::SendEmotion(const std::string &emotion)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 构建消息体：{"emotion": "..."}
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"emotion\":\"%s\"}", emotion.c_str());

    size_t payload_len = strlen(payload);
    uint8_t buffer[PROTOCOL_OVERHEAD + payload_len];

    // 填充帧头
    buffer[0] = PROTOCOL_HEADER_1;
    buffer[1] = PROTOCOL_HEADER_2;
    // 消息类型
    buffer[2] = MSG_TYPE_EMOTION;
    // 数据长度（小端序）
    buffer[3] = payload_len & 0xFF;
    buffer[4] = (payload_len >> 8) & 0xFF;
    // 复制 payload
    memcpy(buffer + 5, payload, payload_len);
    // 计算 CRC8
    buffer[5 + payload_len] = CalculateCRC8(buffer, 5 + payload_len);

    ESP_LOGD(TAG, "Sending emotion: %s", emotion.c_str());
    int sent = uart_write_bytes(uart_port_, buffer, sizeof(buffer));
    return sent == sizeof(buffer);
}

bool UartTransport::SendDeviceState(const std::string &state)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 构建消息体：{"state": "..."}
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", state.c_str());

    size_t payload_len = strlen(payload);
    uint8_t buffer[PROTOCOL_OVERHEAD + payload_len];

    // 填充帧头
    buffer[0] = PROTOCOL_HEADER_1;
    buffer[1] = PROTOCOL_HEADER_2;
    // 消息类型
    buffer[2] = MSG_TYPE_DEVICE_STATE;
    // 数据长度（小端序）
    buffer[3] = payload_len & 0xFF;
    buffer[4] = (payload_len >> 8) & 0xFF;
    // 复制 payload
    memcpy(buffer + 5, payload, payload_len);
    // 计算 CRC8
    buffer[5 + payload_len] = CalculateCRC8(buffer, 5 + payload_len);

    ESP_LOGD(TAG, "Sending device state: %s", state.c_str());
    int sent = uart_write_bytes(uart_port_, buffer, sizeof(buffer));
    return sent == sizeof(buffer);
}

int UartTransport::SendRawData(const uint8_t *data, size_t length)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    ESP_LOGD(TAG, "Sending raw data: %zu bytes", length);
    return uart_write_bytes(uart_port_, data, length);
}
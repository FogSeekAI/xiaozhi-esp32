#include "uart_transport.h"
#include <esp_log.h>
#include <cstring>

#define TAG "UartTransport"

// 新协议格式定义：
// AA 55 | Type | Len_L Len_H | Payload | Checksum | 55 AA

#define PROTOCOL_MIN_FRAME_LEN 8 // Header(2) + Type(1) + Len(2) + Checksum(1) + Footer(2) = 8

UartTransport::UartTransport()
    : uart_port_(UART_NUM_0), tx_pin_(-1), rx_pin_(-1), baud_rate_(0), initialized_(false), receive_task_handle_(nullptr), message_callback_(nullptr) {}

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

// 计算校验和
uint8_t UartTransport::CalculateChecksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;

    for (size_t i = 0; i < length; i++)
    {
        sum += data[i];
    }

    return (uint8_t)(sum & 0xFF);
}

// 任务串口通信测试-------------------------------------------
bool UartTransport::SendData(const uint8_t *data, size_t length)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    int sent = uart_write_bytes(uart_port_, data, length);
    return sent == length;
}

bool UartTransport::SendString(const std::string &str)
{
    return SendData(reinterpret_cast<const uint8_t *>(str.c_str()), str.length());
}

int UartTransport::ReceiveData(uint8_t *buffer, size_t max_length, TickType_t timeout)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    return uart_read_bytes(uart_port_, buffer, max_length, timeout);
}

void UartTransport::UartReceiveTask()
{
    ESP_LOGI(TAG, "UART receive task started (protocol mode - receiver)");

    uint8_t rx_buffer[512];
    size_t rx_index = 0;

    while (true)
    {
        int received = uart_read_bytes(uart_port_, rx_buffer + rx_index, sizeof(rx_buffer) - rx_index, pdMS_TO_TICKS(50));

        if (received > 0)
        {
            rx_index += received;

            while (rx_index >= PROTOCOL_MIN_FRAME_LEN)
            {
                if (rx_buffer[0] != PROTOCOL_HEADER_1 || rx_buffer[1] != PROTOCOL_HEADER_2)
                {
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                uint16_t payload_len = rx_buffer[3] | (rx_buffer[4] << 8);
                size_t frame_len = 2 + 1 + 2 + payload_len + 1 + 2;

                ESP_LOGD(TAG, "Found frame header, payload_len=%d, frame_len=%zu, rx_index=%zu",
                         payload_len, frame_len, rx_index);

                if (rx_index < frame_len)
                {
                    break;
                }

                if (rx_buffer[frame_len - 2] != PROTOCOL_FOOTER_1 ||
                    rx_buffer[frame_len - 1] != PROTOCOL_FOOTER_2)
                {
                    ESP_LOGW(TAG, "Invalid footer");
                    SendErrorResponse(ERROR_LENGTH_ERROR);
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                uint8_t checksum_data[3 + payload_len];
                checksum_data[0] = rx_buffer[2];
                checksum_data[1] = rx_buffer[3];
                checksum_data[2] = rx_buffer[4];
                memcpy(checksum_data + 3, rx_buffer + 5, payload_len);

                uint8_t calculated_checksum = CalculateChecksum(checksum_data, 3 + payload_len);
                uint8_t received_checksum = rx_buffer[5 + payload_len];

                if (calculated_checksum != received_checksum)
                {
                    ESP_LOGW(TAG, "Checksum mismatch");
                    SendErrorResponse(ERROR_CHECKSUM_ERROR);
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                uint8_t msg_type = rx_buffer[2];

                if (msg_type != MSG_TYPE_EMOTION &&
                    msg_type != MSG_TYPE_AUDIO_CONTROL &&
                    msg_type != MSG_TYPE_VOLUME_CONTROL &&
                    msg_type != MSG_TYPE_ACK &&
                    msg_type != MSG_TYPE_ERROR)
                {
                    ESP_LOGW(TAG, "Unknown message type: 0x%02X", msg_type);
                    SendErrorResponse(ERROR_UNKNOWN_TYPE);
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                uint8_t str_len = rx_buffer[5];

                if (str_len > payload_len - 1)
                {
                    ESP_LOGW(TAG, "Invalid string length");
                    SendErrorResponse(ERROR_LENGTH_ERROR);
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                std::string content((char *)(rx_buffer + 6), str_len);

                ESP_LOGI(TAG, "✓ Received: Type=0x%02X, Content=\"%s\"", msg_type, content.c_str());

                // 调用回调函数，将消息类型和内容传递给上层
                if (message_callback_)
                {
                    message_callback_(msg_type, content);
                }

                memmove(rx_buffer, rx_buffer + frame_len, rx_index - frame_len);
                rx_index -= frame_len;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void UartTransport::StartReceiveTask(MessageCallback callback)
{
    if (receive_task_handle_ != nullptr)
    {
        ESP_LOGW(TAG, "Receive task already running");
        return;
    }

    message_callback_ = callback;

    xTaskCreate([](void *arg)
                { static_cast<UartTransport *>(arg)->UartReceiveTask(); }, "uart_receive_task", 4096, this, 5, &receive_task_handle_);

    ESP_LOGI(TAG, "UART receive task created");
}

// 发送应答帧
void UartTransport::SendAckResponse(uint8_t responded_type, uint8_t result_code)
{
    if (!initialized_)
        return;

    uint8_t payload_len = 2;
    uint8_t frame_len = 2 + 1 + 2 + payload_len + 1 + 2;
    uint8_t buffer[frame_len];
    size_t idx = 0;

    buffer[idx++] = PROTOCOL_HEADER_1;
    buffer[idx++] = PROTOCOL_HEADER_2;
    buffer[idx++] = MSG_TYPE_ACK;
    buffer[idx++] = payload_len & 0xFF;
    buffer[idx++] = (payload_len >> 8) & 0xFF;
    buffer[idx++] = responded_type;
    buffer[idx++] = result_code;

    uint8_t checksum_data[3 + payload_len];
    checksum_data[0] = buffer[2];
    checksum_data[1] = buffer[3];
    checksum_data[2] = buffer[4];
    memcpy(checksum_data + 3, buffer + 5, payload_len);
    buffer[idx++] = CalculateChecksum(checksum_data, 3 + payload_len);

    buffer[idx++] = PROTOCOL_FOOTER_1;
    buffer[idx++] = PROTOCOL_FOOTER_2;

    ESP_LOGD(TAG, "Sending ACK: type=0x%02X, result=0x%02X", responded_type, result_code);
    uart_write_bytes(uart_port_, buffer, frame_len);
}

// 发送错误帧
void UartTransport::SendErrorResponse(uint8_t error_code)
{
    if (!initialized_)
        return;

    uint8_t payload_len = 1;
    uint8_t frame_len = 2 + 1 + 2 + payload_len + 1 + 2;
    uint8_t buffer[frame_len];
    size_t idx = 0;

    buffer[idx++] = PROTOCOL_HEADER_1;
    buffer[idx++] = PROTOCOL_HEADER_2;
    buffer[idx++] = MSG_TYPE_ERROR;
    buffer[idx++] = payload_len & 0xFF;
    buffer[idx++] = (payload_len >> 8) & 0xFF;
    buffer[idx++] = error_code;

    uint8_t checksum_data[3 + payload_len];
    checksum_data[0] = buffer[2];
    checksum_data[1] = buffer[3];
    checksum_data[2] = buffer[4];
    memcpy(checksum_data + 3, buffer + 5, payload_len);
    buffer[idx++] = CalculateChecksum(checksum_data, 3 + payload_len);

    buffer[idx++] = PROTOCOL_FOOTER_1;
    buffer[idx++] = PROTOCOL_FOOTER_2;

    ESP_LOGD(TAG, "Sending ERROR: code=0x%02X", error_code);
    uart_write_bytes(uart_port_, buffer, frame_len);
}

//---------------------------------------------------
/* bool UartTransport::SendChatMessage(role_type_t role, const std::string &content)
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
} */

/* bool UartTransport::SendEmotion(const std::string &emotion)
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
} */

/* bool UartTransport::SendDeviceState(const std::string &state)
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
} */

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
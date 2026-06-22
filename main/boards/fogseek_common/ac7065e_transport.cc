#include "ac7065e_transport.h"
#include <esp_log.h>
#include <cstring>

#define TAG "AC7065ETransport"

AC7065ETransport::AC7065ETransport()
    : uart_port_(UART_NUM_0), tx_pin_(-1), rx_pin_(-1), baud_rate_(0),
      initialized_(false), receive_task_handle_(nullptr), message_callback_(nullptr)
{
}

AC7065ETransport::~AC7065ETransport()
{
    if (initialized_)
    {
        uart_driver_delete(uart_port_);
        initialized_ = false;
    }
}

uint8_t AC7065ETransport::CalculateXorChecksum(const uint8_t* data, size_t length)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++)
    {
        checksum ^= data[i];
    }
    return checksum;
}

bool AC7065ETransport::Initialize(uart_port_t uart_port, int tx_pin, int rx_pin, uint32_t baud_rate)
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

    // UART配置参数: 8N1
    uart_config_t uart_config = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(uart_port, &uart_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_set_pin(uart_port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_driver_install(uart_port, 1024, 0, 0, NULL, ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "AC7065E UART initialized on port %d, TX=%d, RX=%d, baud=%lu",
             uart_port, tx_pin, rx_pin, baud_rate);

    return true;
}

void AC7065ETransport::UartReceiveTask()
{
    ESP_LOGI(TAG, "AC7065E UART receive task started");

    static constexpr size_t RX_BUFFER_SIZE = 256;
    uint8_t rx_buffer[RX_BUFFER_SIZE];
    size_t rx_index = 0;

    while (true)
    {
        int received = uart_read_bytes(uart_port_, rx_buffer + rx_index,
                                       sizeof(rx_buffer) - rx_index, pdMS_TO_TICKS(50));

        if (received > 0)
        {
            rx_index += received;

            // 逐字节解析帧
            while (rx_index >= AC7065E_MIN_FRAME_LEN)
            {
                // 查找帧头 0xAA
                if (rx_buffer[0] != AC7065E_HEADER)
                {
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                uint8_t cmd = rx_buffer[1];
                uint8_t data_len = rx_buffer[2];

                // 完整帧长度 = Header(1) + CMD(1) + LEN(1) + DATA(N) + CHK(1)
                size_t frame_len = 1 + 1 + 1 + data_len + 1;

                if (rx_index < frame_len)
                {
                    // 帧不完整，等待更多数据
                    break;
                }

                // 校验 XOR: Header + CMD + LEN + DATA
                uint8_t calculated_chk = CalculateXorChecksum(rx_buffer, 1 + 1 + 1 + data_len);
                uint8_t received_chk = rx_buffer[1 + 1 + 1 + data_len];

                if (calculated_chk != received_chk)
                {
                    ESP_LOGW(TAG, "XOR checksum mismatch: calc=0x%02X, recv=0x%02X, cmd=0x%02X",
                             calculated_chk, received_chk, cmd);

                    // 发送校验错误响应
                    uint8_t error_data[2] = {cmd, ERROR_TYPE_CHECKSUM};
                    SendCommand(CMD_ERROR, error_data, 2);

                    // 跳过当前帧头，继续查找下一个
                    memmove(rx_buffer, rx_buffer + 1, rx_index - 1);
                    rx_index--;
                    continue;
                }

                // 提取 DATA
                const uint8_t* data_ptr = (data_len > 0) ? &rx_buffer[3] : nullptr;

                ESP_LOGI(TAG, "Received frame: cmd=0x%02X, len=%d", cmd, data_len);

                // 调用回调
                if (message_callback_)
                {
                    message_callback_(cmd, data_ptr, data_len);
                }

                // 移除已处理的帧
                memmove(rx_buffer, rx_buffer + frame_len, rx_index - frame_len);
                rx_index -= frame_len;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void AC7065ETransport::StartReceiveTask(MessageCallback callback)
{
    if (receive_task_handle_ != nullptr)
    {
        ESP_LOGW(TAG, "Receive task already running");
        return;
    }

    message_callback_ = callback;

    xTaskCreate(
        [](void *arg)
        { static_cast<AC7065ETransport *>(arg)->UartReceiveTask(); },
        "ac7065e_rx_task", 4096, this, 5, &receive_task_handle_);

    ESP_LOGI(TAG, "AC7065E receive task created");
}

// ============================================================
// 发送命令实现
// ============================================================

bool AC7065ETransport::SendCommand(uint8_t cmd)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 帧: [0xAA] [CMD] [0x00] [CHK]
    uint8_t frame[4];
    frame[0] = AC7065E_HEADER;
    frame[1] = cmd;
    frame[2] = 0x00;  // LEN = 0
    frame[3] = CalculateXorChecksum(frame, 3);  // XOR of Header+CMD+LEN

    int sent = uart_write_bytes(uart_port_, frame, sizeof(frame));
    ESP_LOGD(TAG, "Sent cmd=0x%02X, len=0, chk=0x%02X", cmd, frame[3]);

    return sent == sizeof(frame);
}

bool AC7065ETransport::SendCommand(uint8_t cmd, uint8_t data)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 帧: [0xAA] [CMD] [0x01] [DATA] [CHK]
    uint8_t frame[5];
    frame[0] = AC7065E_HEADER;
    frame[1] = cmd;
    frame[2] = 0x01;  // LEN = 1
    frame[3] = data;
    frame[4] = CalculateXorChecksum(frame, 4);  // XOR of Header+CMD+LEN+DATA

    int sent = uart_write_bytes(uart_port_, frame, sizeof(frame));
    ESP_LOGD(TAG, "Sent cmd=0x%02X, len=1, data=0x%02X, chk=0x%02X", cmd, data, frame[4]);

    return sent == sizeof(frame);
}

bool AC7065ETransport::SendCommand(uint8_t cmd, const uint8_t* data, uint8_t len)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 帧: [0xAA] [CMD] [LEN] [DATA...] [CHK]
    size_t frame_size = 1 + 1 + 1 + len + 1;
    uint8_t* frame = new uint8_t[frame_size];

    frame[0] = AC7065E_HEADER;
    frame[1] = cmd;
    frame[2] = len;

    if (len > 0 && data != nullptr)
    {
        memcpy(&frame[3], data, len);
    }

    // XOR: Header + CMD + LEN + DATA
    frame[frame_size - 1] = CalculateXorChecksum(frame, frame_size - 1);

    int sent = uart_write_bytes(uart_port_, frame, frame_size);
    ESP_LOGD(TAG, "Sent cmd=0x%02X, len=%d, chk=0x%02X", cmd, len, frame[frame_size - 1]);

    delete[] frame;
    return sent == (int)frame_size;
}

bool AC7065ETransport::SendVolumeDefault(uint8_t percent)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 百分比 -> 协议编码:
    //   100% -> 0x00, 90% -> 0x01, 80% -> 0x02, ... 10% -> 0x09, 0% -> 0x10
    uint8_t level;
    if (percent == 0) {
        level = 0x10;
    } else {
        level = (100 - percent) / 10;
    }

    // 音量设置使用裸格式: AA 22 [LEVEL] (无 LEN, 无 CHK)
    uint8_t frame[3];
    frame[0] = AC7065E_HEADER;  // 0xAA
    frame[1] = CMD_VOL_DEFAULT; // 0x22
    frame[2] = level;

    int sent = uart_write_bytes(uart_port_, frame, sizeof(frame));
    ESP_LOGD(TAG, "Sent VOL_DEFAULT: percent=%d, level=0x%02X", percent, level);

    return sent == sizeof(frame);
}

bool AC7065ETransport::SendErrorResponse(uint8_t cmd, uint8_t error_type)
{
    if (!initialized_)
        return false;

    uint8_t data[2] = {cmd, error_type};
    return SendCommand(CMD_ERROR, data, 2);
}

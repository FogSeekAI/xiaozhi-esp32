#include "uart_transport.h"
#include <esp_log.h>
#include <cstring>

#define TAG "UartTransport"

// 新协议格式定义：
// AA 55 | Type | Len_L Len_H | Payload | Checksum | 55 AA

#define PROTOCOL_MIN_FRAME_LEN 8 // Header(2) + Type(1) + Len(2) + Checksum(1) + Footer(2) = 8
// AT 命令相关常量
#define AT_CMD_TIMEOUT 2000       // AT 命令超时时间 (ms)
#define AT_RESPONSE_BUFFER_SIZE 512


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
    
    err = uart_driver_install(uart_port, 1024, 0, 0, NULL, ESP_INTR_FLAG_IRAM);
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

// AT 命令配置相关实现
bool UartTransport::SendATCommand(const std::string& cmd, std::string& response, uint32_t timeout_ms)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    ESP_LOGI(TAG, "Sending AT command: %s", cmd.c_str());
    
    // 清空接收缓冲区
    uart_flush_input(uart_port_);
    
    // 发送 AT 命令
    std::string at_cmd = cmd + "\r\n";
    uart_write_bytes(uart_port_, at_cmd.c_str(), at_cmd.length());
    
    // 读取响应
    char rx_buffer[AT_RESPONSE_BUFFER_SIZE];
    size_t rx_index = 0;
    uint32_t start_time = esp_timer_get_time() / 1000;
    
    // 给模块一些响应时间（特别是第一个字节）
    vTaskDelay(pdMS_TO_TICKS(10));
    
    while ((esp_timer_get_time() / 1000 - start_time) < timeout_ms)
    {
        int received = uart_read_bytes(uart_port_, rx_buffer + rx_index, 1, pdMS_TO_TICKS(50));
        if (received > 0)
        {
            rx_index += received;
            
            // 确保缓冲区以 null 结尾
            if (rx_index < AT_RESPONSE_BUFFER_SIZE)
            {
                rx_buffer[rx_index] = '\0';
            }
            
            // 检查是否包含完整的行（\r\n）
            bool has_complete_line = false;
            for (size_t i = 0; i < rx_index - 1; i++)
            {
                if (rx_buffer[i] == '\r' && rx_buffer[i + 1] == '\n')
                {
                    has_complete_line = true;
                    break;
                }
            }
            
            // 如果收到了完整行或缓冲区快满了，处理响应
            if (has_complete_line || rx_index >= AT_RESPONSE_BUFFER_SIZE - 1)
            {
                // 去除首尾空白字符
                std::string temp_response(rx_buffer);
                size_t start = temp_response.find_first_not_of(" \t\r\n");
                size_t end = temp_response.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos) {
                    response = temp_response.substr(start, end - start + 1);
                } else {
                    response = "";
                }
                
                ESP_LOGI(TAG, "AT response: [%s]", response.empty() ? "(empty)" : response.c_str());
                
                // 关键改进：在整个响应中查找 OK 或 ERROR
                // 使用 strstr 而不是 string::find，因为我们要搜索的是 C 字符串
                
                // 检查是否包含 OK（最终成功标志）
                if (strstr(rx_buffer, "OK") != nullptr)
                {
                    ESP_LOGI(TAG, "AT command successful: %s", cmd.c_str());
                    return true;
                }
                
                // 检查是否包含 ERROR
                if (strstr(rx_buffer, "ERROR") != nullptr)
                {
                    ESP_LOGE(TAG, "AT command failed: %s", cmd.c_str());
                    return false;
                }
                
                // 如果是信息性响应，继续等待 OK
                if (strstr(rx_buffer, "+MQTT") != nullptr ||
                    strstr(rx_buffer, "WIFI") != nullptr ||
                    strstr(rx_buffer, "+C") != nullptr)
                {
                    ESP_LOGD(TAG, "Got informational response, waiting for OK...");
                }
            }
        }
    }
    
    // 超时处理
    ESP_LOGW(TAG, "AT command timeout: %s", cmd.c_str());
    
    // 如果缓冲区有内容，记录最后收到的内容
    if (rx_index > 0)
    {
        rx_buffer[rx_index] = '\0';
        ESP_LOGW(TAG, "Last received: [%s]", rx_buffer);
    }
    
    return false;
}

bool UartTransport::ConfigureWiFiModule()
{
    ESP_LOGI(TAG, "Configuring WiFi module...");
    std::string response;
    
    // 1. 测试通信 - 多次尝试，确保模块准备好
    ESP_LOGI(TAG, "Testing AT communication...");
    bool at_test_success = false;
    for (int retry = 0; retry < 3; retry++)
    {
        if (SendATCommand("AT", response))
        {
            at_test_success = true;
            ESP_LOGI(TAG, "AT test successful (attempt %d)", retry + 1);
            break;
        }
        ESP_LOGW(TAG, "AT test failed (attempt %d), retrying...", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (!at_test_success)
    {
        ESP_LOGE(TAG, "AT test failed after 3 attempts");
        return false;
    }
    
    // 2. 关闭回显
    ESP_LOGI(TAG, "Disabling AT echo...");
    SendATCommand("ATE0", response);
    
    // 3. 设置 WiFi 模式为 Station 模式
    ESP_LOGI(TAG, "Setting WiFi mode to Station...");
    if (!SendATCommand("AT+CWMODE=1", response))
    {
        ESP_LOGE(TAG, "Failed to set WiFi mode");
        return false;
    }
    ESP_LOGI(TAG, "WiFi mode set successfully");
    
    // 注意：MQTTUSERCFG 不在这里执行，而是在 ConfigureMQTT 中执行
    // 因为需要等待 WiFi 连接后才能配置
    
    return true;
}

bool UartTransport::ConnectToWiFi(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
    
    // 先断开之前的连接
    ESP_LOGI(TAG, "Disconnecting from previous AP...");
    std::string response;
    SendATCommand("AT+CWQAP", response);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    std::string cmd = "AT+CWJAP=\"" + std::string(ssid) + "\",\"" + std::string(password) + "\"";
    
    // WiFi 连接需要更长时间，并且要等待 WIFI GOT IP 响应
    uint32_t timeout_ms = 15000; // 15 秒
    uint32_t start_time = esp_timer_get_time() / 1000;
    
    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout: %lu ms)...", timeout_ms);
    ESP_LOGI(TAG, "Sending AT command: %s", cmd.c_str());
    
    // 清空接收缓冲区
    uart_flush_input(uart_port_);
    
    // 发送 AT 命令
    uart_write_bytes(uart_port_, (cmd + "\r\n").c_str(), cmd.length() + 2);
    
    bool connected = false;
    bool got_ok = false;
    
    while ((esp_timer_get_time() / 1000 - start_time) < timeout_ms)
    {
        char rx_buffer[256];
        int received = uart_read_bytes(uart_port_, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(200));
        
        if (received > 0)
        {
            rx_buffer[received] = '\0';
            
            // 逐行处理响应
            char *line = strtok(rx_buffer, "\r\n");
            while (line != nullptr)
            {
                ESP_LOGI(TAG, "WiFi status: %s", line);
                
                // 检查是否连接成功
                if (strstr(line, "WIFI GOT IP") != nullptr)
                {
                    ESP_LOGI(TAG, "✓ WiFi got IP address");
                    connected = true;
                }
                
                if (strstr(line, "WIFI CONNECTED") != nullptr)
                {
                    ESP_LOGI(TAG, "✓ WiFi connected to AP");
                }
                
                // 忽略 "WIFI DISCONNECT"，这可能是断开之前的连接
                if (strstr(line, "WIFI DISCONNECT") != nullptr)
                {
                    ESP_LOGI(TAG, "ℹ WiFi disconnected (previous connection)");
                }
                
                // 检查 OK（必须在 WIFI GOT IP 之后）
                if (strstr(line, "OK") != nullptr)
                {
                    ESP_LOGI(TAG, "✓ AT command completed with OK");
                    got_ok = true;
                }
                
                // 检查失败
                if (strstr(line, "FAIL") != nullptr ||
                    strstr(line, "ERROR") != nullptr)
                {
                    ESP_LOGE(TAG, "✗ WiFi connection failed");
                    return false;
                }
                
                // 检查特定错误
                if (strstr(line, "NO AP") != nullptr)
                {
                    ESP_LOGE(TAG, "✗ AP not found - check SSID");
                    return false;
                }
                
                if (strstr(line, "PASSWORD INCORRECT") != nullptr ||
                    strstr(line, "AUTH FAIL") != nullptr)
                {
                    ESP_LOGE(TAG, "✗ Password incorrect");
                    return false;
                }
                
                line = strtok(nullptr, "\r\n");
            }
        }
        
        // 如果已经收到 OK 且已连接，退出
        if (connected && got_ok)
        {
            ESP_LOGI(TAG, "✓ WiFi connection successful!");
            return true;
        }
    }
    
    ESP_LOGE(TAG, "✗ WiFi connection timeout");
    return false;
}

bool UartTransport::ConfigureMQTT(const char* client_id, const char* server_addr, int port)
{
    ESP_LOGI(TAG, "Configuring MQTT on ESP-15F...");
    std::string response;
    
    // 等待 WiFi 连接稳定
    ESP_LOGI(TAG, "Waiting for WiFi connection to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 1. 使用 AT+MQTTUSERCFG 配置 MQTT 参数（Client ID 保持为 NULL）
    ESP_LOGI(TAG, "Setting MQTT user configuration...");
    std::string cmd = "AT+MQTTUSERCFG=0,1,\"NULL\",\"\",\"\",0,0,\"\"";
    SendATCommand(cmd, response);
    
    // 3. 连接 MQTT 服务器
    ESP_LOGI(TAG, "Connecting to MQTT: %s:%d", server_addr, port);
    cmd = "AT+MQTTCONN=0,\"" + std::string(server_addr) + "\"," + std::to_string(port) + ",1";
    
    bool mqtt_conn_success = false;
    for (int i = 0; i < 3; i++)
    {
        if (SendATCommand(cmd, response, 10000))
        {
            mqtt_conn_success = true;
            ESP_LOGI(TAG, "MQTT connected on attempt %d", i + 1);
            break;
        }
        ESP_LOGW(TAG, "MQTT connection attempt %d failed, retrying...", i + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (!mqtt_conn_success)
    {
        ESP_LOGE(TAG, "Failed to connect to MQTT server after all attempts");
        return false;
    }
    
    ESP_LOGI(TAG, "✓ MQTT configuration complete!");
    return true;
}


bool UartTransport::TestMQTTConnection()
{
    ESP_LOGI(TAG, "Testing MQTT connection...");
    std::string response;
    
    // 发布测试消息
    ESP_LOGI(TAG, "Publishing test message to esp15f/test");
    if (!SendATCommand("AT+MQTTPUB=0,\"esp15f/test\",\"Hello from ESP15F\",1,0", response, 5000))
    {
        ESP_LOGE(TAG, "MQTT publish test failed");
        return false;
    }
    
    ESP_LOGI(TAG, "MQTT connection test successful");
    return true;
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

// 辅助函数：将字符串转换为十六进制字符串
static std::string StringToHex(const std::string& str)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(str.length() * 2);
    
    for (char c : str)
    {
        result += hex_chars[(c >> 4) & 0x0F];
        result += hex_chars[c & 0x0F];
    }
    
    return result;
}

// 辅助函数：转义字符串中的双引号和反斜杠
static std::string EscapeJsonString(const std::string& str)
{
    std::string result;
    result.reserve(str.length() * 2);
    
    for (char c : str)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
        }
        result += c;
    }
    
    return result;
}

bool UartTransport::SendChatMessage(role_type_t role, const std::string& content)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 构建简化格式的 payload，避免双引号
    // 格式：role|content (例如：1|你好，小智。)
    char payload[512];
    snprintf(payload, sizeof(payload), "%d|%s", role, content.c_str());

    // 使用 AT+MQTTPUB 发布
    std::string cmd = "AT+MQTTPUB=0,\"esp15f/test\",\"" + std::string(payload) + "\",1,0";
    std::string response;
    
    ESP_LOGI(TAG, "Publishing chat message via MQTT: %s", payload);
    bool success = SendATCommand(cmd, response, 5000);
    
    if (success) {
        ESP_LOGI(TAG, "MQTT publish successful");
    } else {
        ESP_LOGE(TAG, "MQTT publish failed, response: %s", response.c_str());
    }
    
    return success;
}

bool UartTransport::SendEmotion(const std::string& emotion)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // 构建简化格式的 payload: emotion_value (例如：happy)
    // 直接使用 emotion 字符串，不需要额外格式
    
    // 使用 AT+MQTTPUB 发布
    std::string cmd = "AT+MQTTPUB=0,\"esp15f/test\",\"" + emotion + "\",1,0";
    std::string response;
    
    ESP_LOGI(TAG, "Publishing emotion via MQTT: %s", emotion.c_str());
    bool success = SendATCommand(cmd, response, 5000);
    
    if (success) {
        ESP_LOGI(TAG, "MQTT publish successful");
    } else {
        ESP_LOGE(TAG, "MQTT publish failed, response: %s", response.c_str());
    }
    
    return success;
}


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
#pragma once

#include <driver/uart.h>
#include <string>
#include <cstdint>
#include <functional>

#define PROTOCOL_HEADER_1       0xAA
#define PROTOCOL_HEADER_2       0x55
#define PROTOCOL_FOOTER_1       0x55
#define PROTOCOL_FOOTER_2       0xAA

/**
 * @brief 消息类型枚举
 */
typedef enum {
    MSG_TYPE_EMOTION = 0x01, //情绪信息
    MSG_TYPE_AUDIO_CONTROL = 0x02, // 音频控制命令
    MSG_TYPE_VOLUME_CONTROL = 0x03, // 音量控制命令
    MSG_TYPE_ACK = 0x05,     //应答
    MSG_TYPE_ERROR = 0x06,   //错误信息
} message_type_t;

typedef enum {
    ACK_RESULT_SUCCESS = 0x00,
    ACK_RESULT_FAILURE = 0x01,
} ack_result_t;

typedef enum {
    ERROR_UNKNOWN_TYPE = 0x01,
    ERROR_LENGTH_ERROR = 0x02,
    ERROR_CHECKSUM_ERROR = 0x03,
    ERROR_UNSUPPORTED_EMOTION = 0x04,
    ERROR_WAV_SEQUENCE_ERROR = 0x05,
    ERROR_WAV_LENGTH_ERROR = 0x06,
    ERROR_DEVICE_BUSY = 0x07,
    ERROR_FILENAME_ERROR = 0x08,
    ERROR_FILESIZE_ERROR = 0x09,
    ERROR_UNSUPPORTED_ANIMAL = 0x0A,
    ERROR_INVALID_VOLUME_CMD = 0x0B,
} error_code_t;

/**
 * @brief 聊天消息角色类型
 */
typedef enum {
    ROLE_USER = 0x01,      // 用户消息
    ROLE_ASSISTANT = 0x02, // AI 助手消息
    ROLE_SYSTEM = 0x03,    // 系统消息
} role_type_t;

/**
 * @brief UART串口传输类
 * @brief 用于封装语音对话内容和情绪信息，通过UART 发送给ESP-15F透传模块
 */
class UartTransport {
private:
    uart_port_t uart_port_;
    int tx_pin_;
    int rx_pin_;
    uint32_t baud_rate_;
    bool initialized_;

    //接收缓冲区
    static constexpr size_t RX_BUFFER_SIZE = 256;
    uint8_t rx_buffer_[RX_BUFFER_SIZE];
    
    // 接收任务句柄
    TaskHandle_t receive_task_handle_;

    // 接收回调函数类型 - 传递消息类型和内容
    using MessageCallback = std::function<void(uint8_t msg_type, const std::string& content)>;
    MessageCallback message_callback_;

    void UartReceiveTask();

    

    /**
     * @brief 计算 CRC8校验值
     * @param data 数据指针
     * @param length 数据长度
     * @return CRC8校验值
     */
    uint8_t CalculateCRC8(const uint8_t* data, size_t length);

    uint8_t CalculateChecksum(const uint8_t* data, size_t length);

    /**
     * @brief 发送 ACK 应答帧
     * @param content 应答内容
     * @return 是否成功
     */
    


public:
    UartTransport();
    ~UartTransport();

    /**
     * @brief 初始化 UART串口
     * @param uart_port UART端口号（如UART_NUM_0）
     * @param tx_pin TX引脚编号
     * @param rx_pin RX引脚编号
     * @param baud_rate 波特率
     * @return 是否成功
     */
    bool Initialize(uart_port_t uart_port, int tx_pin, int rx_pin, uint32_t baud_rate);

    //
    bool SendData(const uint8_t* data, size_t length);
    bool SendString(const std::string& str);

    int ReceiveData(uint8_t* buffer, size_t max_length, TickType_t timeout = pdMS_TO_TICKS(100));

    void StartReceiveTask(MessageCallback callback);
    
    /**
     * @brief 设置接收回调函数
     * @param callback 回调函数
     */
    
    void SendAckResponse(uint8_t responded_type, uint8_t result_code);
    

    /**
     * @brief 发送 ERROR 错误帧
     * @param error 错误信息
     * @return 是否成功
     */
    void SendErrorResponse(uint8_t error_code);
    
    //

    /**
     * @brief 发送聊天消息
     * @param role 消息角色（user/assistant/system）
     * @param content 消息内容
     * @return 是否成功
     */
    bool SendChatMessage(role_type_t role, const std::string& content);

    /**
     * @brief 发送情绪信息
     * @param emotion 情绪字符串（如"happy", "sad", "neutral"等）
     * @return 是否成功
     */
    bool SendEmotion(const std::string& emotion);

    /**
     * @brief 发送设备状态
     * @param state 设备状态字符串
     * @return 是否成功
     */
    bool SendDeviceState(const std::string& state);

    /**
     * @brief 直接发送原始数据（用于调试）
     * @param data 数据指针
     * @param length 数据长度
     * @return 发送的字节数
     */
    int SendRawData(const uint8_t* data, size_t length);
};
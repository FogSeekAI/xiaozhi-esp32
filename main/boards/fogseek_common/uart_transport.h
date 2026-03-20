#pragma once

#include <driver/uart.h>
#include <string>
#include <cstdint>

/**
 * @brief 消息类型枚举
 */
typedef enum {
    MSG_TYPE_CHAT_MESSAGE = 0x01,  // 聊天消息
    MSG_TYPE_EMOTION = 0x02,       // 情绪信息
    MSG_TYPE_DEVICE_STATE = 0x03,  // 设备状态
} message_type_t;

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

    /**
     * @brief 计算 CRC8校验值
     * @param data 数据指针
     * @param length 数据长度
     * @return CRC8校验值
     */
    uint8_t CalculateCRC8(const uint8_t* data, size_t length);

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
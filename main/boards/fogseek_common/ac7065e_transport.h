#pragma once

#include <driver/uart.h>
#include <string>
#include <cstdint>
#include <functional>

// AC7065E 协议常量
#define AC7065E_HEADER          0xAA
#define AC7065E_MIN_FRAME_LEN   4   // Header(1) + CMD(1) + LEN(1) + CHK(1) = 4

// ============================================================
// ESP32 -> AC7065E 命令 (主控下发)
// ============================================================
#define CMD_WAKEUP              0x01  // 进入AI语音对话模式
#define CMD_SLEEP               0x02  // 关闭AI语音对话，切换蓝牙模式
#define CMD_PLAY                0x10  // 恢复蓝牙音乐播放
#define CMD_PAUSE               0x11  // 暂停蓝牙音乐
#define CMD_NEXT_TRACK          0x12  // 下一曲
#define CMD_PREV_TRACK          0x13  // 上一曲
#define CMD_VOL_UP              0x20  // 音量加
#define CMD_VOL_DOWN            0x21  // 音量减
#define CMD_VOL_DEFAULT         0x22  // 设置默认音量
#define CMD_VOL_MAX             0x23  // 最大音量
#define CMD_VOL_MIN             0x24  // 最小音量（或静音）
#define CMD_GET_BATTERY         0x30  // 查询电量
#define CMD_GET_VOL             0x31  // 查询音量

// ============================================================
// AC7065E -> ESP32 上报命令 (设备上报)
// ============================================================
#define CMD_POWER_ON            0x80  // 开关机状态上报
#define CMD_AUX_INSERT          0x81  // 物理AUX线插入
#define CMD_AUX_REMOVE          0x82  // 物理AUX线拔出
#define CMD_CMD_ACK             0x83  // 确认收到某条指令
#define CMD_BATTERY_RESP        0x84  // 电量百分比回复
#define CMD_VOL_RESP            0x85  // 音量百分比回复
#define CMD_ERROR               0xFF  // 错误

// 错误类型码
#define ERROR_TYPE_FAILED       0x00  // 失败
#define ERROR_TYPE_TIMEOUT      0x01  // 超时
#define ERROR_TYPE_CHECKSUM     0x10  // 校验错误

/**
 * @brief AC7065E 蓝牙音频协处理器 UART 通信传输类
 *
 * 协议帧格式: [0xAA] [CMD] [LEN] [DATA...] [CHK]
 * 校验方式: 帧头 + CMD + LEN + DATA 所有字节的 XOR
 */
class AC7065ETransport {
private:
    uart_port_t uart_port_;
    int tx_pin_;
    int rx_pin_;
    uint32_t baud_rate_;
    bool initialized_;

    // 接收任务句柄
    TaskHandle_t receive_task_handle_;

    // 接收回调: (cmd, data, len)
    using MessageCallback = std::function<void(uint8_t cmd, const uint8_t* data, uint8_t len)>;
    MessageCallback message_callback_;

    /**
     * @brief 计算 XOR 校验和
     * @param data 数据指针
     * @param length 数据长度
     * @return XOR 校验值
     */
    uint8_t CalculateXorChecksum(const uint8_t* data, size_t length);

    /**
     * @brief UART 接收任务主循环
     */
    void UartReceiveTask();

public:
    AC7065ETransport();
    ~AC7065ETransport();

    /**
     * @brief 初始化 UART 串口
     * @param uart_port UART端口号
     * @param tx_pin TX引脚编号
     * @param rx_pin RX引脚编号
     * @param baud_rate 波特率 (默认115200)
     * @return 是否成功
     */
    bool Initialize(uart_port_t uart_port, int tx_pin, int rx_pin, uint32_t baud_rate = 115200);

    /**
     * @brief 启动接收任务
     * @param callback 接收到完整帧时的回调
     */
    void StartReceiveTask(MessageCallback callback);

    // ============================================================
    // 发送命令 (ESP32 -> AC7065E)
    // ============================================================

    /** @brief 发送无数据命令 */
    bool SendCommand(uint8_t cmd);

    /** @brief 发送带单字节数据的命令 */
    bool SendCommand(uint8_t cmd, uint8_t data);

    /** @brief 发送带多字节数据的命令 */
    bool SendCommand(uint8_t cmd, const uint8_t* data, uint8_t len);

    // ---- 便捷发送函数 ----

    /** @brief 进入AI语音对话模式 */
    bool SendWakeUp() { return SendCommand(CMD_WAKEUP); }

    /** @brief 关闭AI语音对话，切换蓝牙模式 */
    bool SendSleep() { return SendCommand(CMD_SLEEP); }

    /** @brief 恢复蓝牙音乐播放 */
    bool SendPlay() { return SendCommand(CMD_PLAY); }

    /** @brief 暂停蓝牙音乐 */
    bool SendPause() { return SendCommand(CMD_PAUSE); }

    /** @brief 下一曲 */
    bool SendNextTrack() { return SendCommand(CMD_NEXT_TRACK); }

    /** @brief 上一曲 */
    bool SendPrevTrack() { return SendCommand(CMD_PREV_TRACK); }

    /** @brief 音量加 */
    bool SendVolumeUp() { return SendCommand(CMD_VOL_UP); }

    /** @brief 音量减 */
    bool SendVolumeDown() { return SendCommand(CMD_VOL_DOWN); }

    /**
     * @brief 设置默认音量 (百分比 0-100)
     * @param percent 音量百分比
     *
     * 协议映射: 0x00=100%, 0x01=90%, 0x02=80%, ... 0x09=10%, 0x10=0%
     */
    bool SendVolumeDefault(uint8_t percent);

    /** @brief 最大音量 */
    bool SendVolumeMax() { return SendCommand(CMD_VOL_MAX); }

    /** @brief 最小音量/静音 */
    bool SendVolumeMin() { return SendCommand(CMD_VOL_MIN); }

    /** @brief 查询电量 */
    bool SendGetBattery() { return SendCommand(CMD_GET_BATTERY); }

    /** @brief 查询音量 */
    bool SendGetVolume() { return SendCommand(CMD_GET_VOL); }

    /**
     * @brief 发送错误响应 (ESP32 -> AC7065E)
     * @param cmd 原始命令类型
     * @param error_type 错误类型
     */
    bool SendErrorResponse(uint8_t cmd, uint8_t error_type);
};

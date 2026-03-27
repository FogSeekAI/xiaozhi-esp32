#ifndef _TCA6408A_LED_CONTROLLER_H_
#define _TCA6408A_LED_CONTROLLER_H_

#include "tca6408a_io_expander.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include "device_state.h"
#include "led/gpio_led.h"
#include "led/led.h"
#include "led/circular_strip.h"
#include <memory>
#include <mutex>

/**
 * @brief LED 引脚配置结构体
 */
typedef struct
{
    int rgb_gpio = -1;        // RGB 灯带 GPIO
    int rgb_num_leds = 0;     // RGB LED 数量
    int cold_light_gpio = -1; // 冷色灯 GPIO
    int warm_light_gpio = -1; // 暖色灯 GPIO
} led_pin_config_t;

/**
 * @brief 基于 TCA6408A IO 扩展器的 LED 控制器
 * 
 * 功能特性:
 * - 通过 TCA6408A 控制红绿灯 (P2, P3)
 * - 原生 GPIO 控制冷暖色灯和 RGB 灯带
 * - 支持设备状态显示
 * - 支持电源状态显示
 */
class TCA6408ALedController
{
public:
    /**
     * @brief 构造函数
     */
    TCA6408ALedController();
    
    /**
     * @brief 析构函数
     */
    ~TCA6408ALedController();

    /**
     * @brief 初始化 LED 控制器
     * 
     * @param pin_config LED 引脚配置
     * @param i2c_bus I2C 总线句柄
     * @param i2c_address TCA6408A I2C 地址（默认 0x20）
     * @return esp_err_t 错误代码
     */
    esp_err_t Initialize(const led_pin_config_t *pin_config, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address = 0x20);

    /**
     * @brief 更新 LED 状态（根据设备状态和电源状态）
     * 
     * @param device_state 设备状态
     * @param power_state 电源状态
     * @param device_power_state 设备电源开关状态
     */
    void UpdateLedStatus(DeviceState device_state, 
                        TCA6408APowerManager::PowerState power_state,
                        TCA6408APowerManager::DevicePowerState device_power_state);

    /**
     * @brief 设置红色 LED
     * @param on true=亮，false=灭
     */
    void SetRedLed(bool on);

    /**
     * @brief 设置绿色 LED
     * @param on true=亮，false=灭
     */
    void SetGreenLed(bool on);

    /**
     * @brief 控制冷色灯
     * @param state true=开启，false=关闭
     */
    void SetColdLight(bool state);

    /**
     * @brief 控制暖色灯
     * @param state true=开启，false=关闭
     */
    void SetWarmLight(bool state);

    /**
     * @brief 设置冷色灯亮度
     * @param brightness 亮度值 (0-100)
     */
    void SetColdLightBrightness(int brightness);

    /**
     * @brief 设置暖色灯亮度
     * @param brightness 亮度值 (0-100)
     */
    void SetWarmLightBrightness(int brightness);

    /**
     * @brief 获取冷色灯状态
     */
    bool IsColdLightOn() const { return cold_light_state_; }

    /**
     * @brief 获取暖色灯状态
     */
    bool IsWarmLightOn() const { return warm_light_state_; }

    /**
     * @brief 获取 RGB 灯带实例
     */
    RgbLedStrip *GetRgbLedStrip() const { return rgb_led_strip_; }

    /**
     * @brief 获取 RGB LED 数量
     */
    uint8_t GetNumLeds() const { return num_leds_; }

private:
    static const char *TAG;

    // 成员变量
    tca6408a_handle_t io_expander_;           // IO 扩展器句柄
    led_pin_config_t pin_config_;             // LED 引脚配置
    
    // 原生 GPIO 控制的 LED
    GpioLed *cold_light_ = nullptr;           // 冷色灯
    GpioLed *warm_light_ = nullptr;           // 暖色灯
    RgbLedStrip *rgb_led_strip_ = nullptr;    // RGB 灯带
    uint8_t num_leds_ = 0;                    // RGB LED 数量
    
    // 状态标志
    bool cold_light_state_ = false;           // 冷色灯状态
    bool warm_light_state_ = false;           // 暖色灯状态
    bool green_ignore_state_ = false;         // 绿灯是否忽略状态变化
    
    mutable std::mutex mutex_;                // 互斥锁

    // 私有辅助方法
    void StartGreenBreathe();
    void StartGreenContinuousBlink(int interval_ms);
};

#endif

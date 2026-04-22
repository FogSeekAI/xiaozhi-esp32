#ifndef _FOGSEEK_LED_CONTROLLER_H_
#define _FOGSEEK_LED_CONTROLLER_H_

#include <driver/gpio.h>
#include <esp_timer.h>
#include "device_state.h"
#include "led/gpio_led.h"
#include "led/led.h"
#include "power_manager.h"
#include "led/circular_strip.h"

#include <memory>
#include <mutex>

// LED 引脚配置结构体
typedef struct
{
    int red_gpio;             // 红色 LED GPIO 引脚
    int green_gpio;           // 绿色 LED GPIO 引脚
    int rgb_gpio = -1;        // RGB 灯带 GPIO，默认为 -1 表示不使用
    int rgb_num_leds = 0;     // RGB 灯带 LED 数量，默认为 0
    int cold_light_gpio = -1; // 冷色灯 GPIO，默认为 -1 表示不使用
    int warm_light_gpio = -1; // 暖色灯 GPIO，默认为 -1 表示不使用
} led_pin_config_t;

/**
 * @brief 红色LED类，负责电源状态显示
 */
class RedLed : public GpioLed
{
public:
    RedLed(gpio_num_t gpio);
    RedLed(gpio_num_t gpio, int output_invert, ledc_timer_t timer_num, ledc_channel_t channel);
    ~RedLed();

    // 空实现，因为红灯不响应设备状态变化
    void OnStateChanged() override;

    // 响应电源状态变化
    void UpdateBatteryStatus(FogSeekPowerManager::PowerState state);

private:
    static const char *TAG;
};

/**
 * @brief 绿色LED类，负责设备状态显示
 */
class GreenLed : public GpioLed
{
public:
    GreenLed(gpio_num_t gpio);
    GreenLed(gpio_num_t gpio, int output_invert, ledc_timer_t timer_num, ledc_channel_t channel);
    ~GreenLed();

    // 响应设备状态变化
    void OnStateChanged() override;

    // 设置是否忽略设备状态变化
    void SetIgnoreStateChanges(bool ignore) { ignore_state_changes_ = ignore; }
    bool IsIgnoringStateChanges() const { return ignore_state_changes_; }

private:
    static const char *TAG;
    bool ignore_state_changes_ = false; // 是否忽略设备状态变化
};

/**
 * @brief 扩展的 RGB 灯带类
 * 继承自 CircularStrip，使用装饰器模式增强颜色管理功能
 */
class RgbLedStrip : public CircularStrip
{
public:
    RgbLedStrip(gpio_num_t gpio, uint8_t num_leds);

    // 装饰器模式方法并记录当前颜色
    void SetAllColor(StripColor color);
    void SetSingleColor(uint8_t index, StripColor color);
    void Blink(StripColor color, int interval_ms);
    void Scroll(StripColor low, StripColor high, int length, int interval_ms);

    // 重写灯光特效
    void StartBreathe(int breath_time_ms);                 // 呼吸效果
    void TurnOnStrip(int total_time_ms, StripColor color); // 开灯效果
    void TurnOffStrip(int total_time_ms);                  // 熄灯效果

    // 亮度等级控制
    void IncreaseBrightness();
    void DecreaseBrightness();

private:
    static const char *TAG;
    StripColor current_color_ = {255, 0, 0}; // 原始颜色（不受亮度影响）
    uint8_t num_leds_ = 0;
    uint8_t brightness_level_ = 3;
    void ApplyBrightness();
};

// 用于区分不同效果的枚举 - 需要在类外部声明以供内部使用
enum EffectType
{
    NONE,
    MARQUEE_EFFECT,
    TURN_ON_LIGHTS,
    TURN_OFF_LIGHTS,
    BREATHING_EFFECT
};

/**
 * @brief 雾岸设备 LED 控制器类
 *
 * 该类是 LED 系统的主控制器，负责管理红绿灯和其他 LED 设备。
 * 内部使用 RedLed 和 GreenLed 类分别控制红灯和绿灯，这些是
 * 内部实现细节，外部代码应通过本类的公共接口进行操作。
 */
class FogSeekLedController
{
public:
    FogSeekLedController();
    ~FogSeekLedController();

    // 初始化LED GPIO
    void InitializeLeds(FogSeekPowerManager &power_manager, const led_pin_config_t *pin_config);

    // 更新 LED 状态
    void UpdateLedStatus(FogSeekPowerManager &power_manager);

    // 冷暖色灯控制
    void SetColdLight(bool state);
    void SetWarmLight(bool state);
    void SetColdLightBrightness(int brightness);
    void SetWarmLightBrightness(int brightness);
    bool IsColdLightOn() const { return cold_light_state_; }
    bool IsWarmLightOn() const { return warm_light_state_; }

    // RGB 灯带控制
    RgbLedStrip *GetRgbLedStrip() const { return rgb_led_strip_; }
    uint8_t GetNumLeds() const { return num_leds_; }

    // 获取 LED 实例的方法
    RedLed *GetRedLed() const { return red_led_; }
    GreenLed *GetGreenLed() const { return green_led_; }
    GpioLed *GetColdLight() const { return cold_light_; }
    GpioLed *GetWarmLight() const { return warm_light_; }

private:
    static const char *TAG; // 日志标签

    // LED 实例
    RedLed *red_led_ = nullptr;     // 红色 LED 控制器实例
    GreenLed *green_led_ = nullptr; // 绿色 LED 控制器实例

    // 冷暖色灯控制
    GpioLed *cold_light_ = nullptr; // 冷色灯控制器实例
    GpioLed *warm_light_ = nullptr; // 暖色灯控制器实例
    bool cold_light_state_ = false; // 冷色灯当前状态
    bool warm_light_state_ = false; // 暖色灯当前状态

    // RGB 灯带控制
    RgbLedStrip *rgb_led_strip_ = nullptr; // RGB 灯带控制器实例
    uint8_t num_leds_ = 0;                 // RGB LED 数量

    led_pin_config_t pin_config_; // LED 引脚配置

    mutable std::mutex mutex_; // 保护 RGB 灯带操作的互斥锁
};

#endif
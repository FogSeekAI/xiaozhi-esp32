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

// LED引脚配置结构体
typedef struct
{
    int red_gpio;             // 红色LED GPIO引脚
    int green_gpio;           // 绿色LED GPIO引脚
    int rgb_gpio = -1;        // RGB灯带GPIO，默认为-1表示不使用
    int cold_light_gpio = -1; // 冷色灯GPIO，默认为-1表示不使用
    int warm_light_gpio = -1; // 暖色灯GPIO，默认为-1表示不使用
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
 * @brief 雾岸设备LED控制器类
 *
 * 该类是LED系统的主控制器，负责管理红绿灯和其他LED设备。
 * 内部使用RedLed和GreenLed类分别控制红灯和绿灯，这些是
 * 内部实现细节，外部代码应通过本类的公共接口进行操作。
 */
class FogSeekLedController
{
public:
    FogSeekLedController();
    ~FogSeekLedController();

    // 初始化LED GPIO
    void InitializeLeds(FogSeekPowerManager &power_manager, const led_pin_config_t *pin_config);

    // 更新LED状态
    void UpdateLedStatus(FogSeekPowerManager &power_manager);

    // 冷暖色灯控制
    void SetColdLight(bool state);
    void SetWarmLight(bool state);
    void SetColdLightBrightness(int brightness);
    void SetWarmLightBrightness(int brightness);
    bool IsColdLightOn() const { return cold_light_state_; }
    bool IsWarmLightOn() const { return warm_light_state_; }

    // RGB灯带控制方法
    void SetRgbStrip(CircularStrip *strip, uint8_t num_leds); // 设置RGB灯带实例
    bool RunMarqueeLights(int total_time_ms = 5000);          // 跑马灯效果，total_time_ms毫秒内依次点亮所有灯，原PowerOnSequence函数改名
    bool TurnOnRgbLights(int duration_ms = 1000);             // 打开所有灯光，duration_ms时间内从暗到亮
    bool TurnOffRgbLights(int duration_ms = 1000);            // 关闭所有灯光，duration_ms时间内从亮到暗
    void IncreaseBrightness();                                // 增加亮度一个档位
    void DecreaseBrightness();                                // 降低亮度一个档位
    void ChangeToRandomColors();                              // 随机变化颜色（红橙黄绿青蓝紫）
    bool StartBreathingEffect(int cycle_duration_ms = 4000);  // 开始呼吸效果，cycle_duration_ms为一个完整周期的时间
    void StopBreathingEffect();                               // 停止呼吸效果

    // 获取LED实例的方法
    RedLed *GetRedLed() const { return red_led_; }
    GreenLed *GetGreenLed() const { return green_led_; }
    GpioLed *GetColdLight() const { return cold_light_; }
    GpioLed *GetWarmLight() const { return warm_light_; }
    CircularStrip *GetRgbStrip() const { return rgb_led_strip_; }

    // 定时器状态查询
    bool IsEffectRunning() const { return is_effect_running_; }
    bool IsBreathingEffectActive() const { return current_effect_type_ == BREATHING_EFFECT; }

    // 内部辅助函数（需要被静态回调函数访问）
    void EffectTimerCallback(); // 效果定时器回调函数

private:
    static const char *TAG; // 日志标签

    // LED实例
    RedLed *red_led_ = nullptr;     // 红色LED控制器实例
    GreenLed *green_led_ = nullptr; // 绿色LED控制器实例

    // 冷暖色灯控制
    GpioLed *cold_light_ = nullptr; // 冷色灯控制器实例
    GpioLed *warm_light_ = nullptr; // 暖色灯控制器实例
    bool cold_light_state_ = false; // 冷色灯当前状态
    bool warm_light_state_ = false; // 暖色灯当前状态

    // RGB灯带控制
    CircularStrip *rgb_led_strip_ = nullptr;               // RGB灯带控制器实例
    uint8_t rgb_num_leds_ = 0;                             // RGB灯珠数量
    uint8_t current_brightness_level_ = 2;                 // 当前亮度等级 (0-4)，默认为中间值
    uint8_t brightness_levels_[5] = {10, 30, 50, 70, 100}; // 亮度等级对应的百分比
    StripColor current_color_ = {255, 255, 255};           // 当前颜色，用于平滑过渡
    StripColor original_color_ = {255, 255, 255};          // 原始颜色，用于亮度调节

    // 通用异步效果相关
    esp_timer_handle_t effect_timer_ = nullptr;
    bool is_effect_running_ = false;

    // 通用效果参数
    EffectType current_effect_type_ = NONE; // 当前效果类型
    int effect_duration_ms_ = 0;            // 效果持续时间
    int effect_delay_per_step_ = 0;         // 每步延迟时间
    int effect_current_step_ = 0;           // 当前步骤
    int effect_total_steps_ = 0;            // 总步骤数

    // 呼吸效果专用参数
    float breathing_direction_ = 1.0f; // 呼吸方向：1为增强，-1为减弱

    led_pin_config_t pin_config_; // LED引脚配置

    // 内部辅助函数
    bool StartEffect(EffectType type, int duration_ms); // 启动通用效果
    void StopCurrentEffect();                           // 停止当前效果
};

#endif
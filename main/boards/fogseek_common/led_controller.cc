#include "led_controller.h"
#include "power_manager.h"
#include "../../application.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <math.h>
#include <memory>
#include <algorithm>

/// 日志标签
const char *FogSeekLedController::TAG = "FogSeekLedController";
const char *RedLed::TAG = "RedLed";
const char *GreenLed::TAG = "GreenLed";
const char *RgbLedStrip::TAG = "RgbLedStrip";

// ==================== RedLed Implementation ====================
RedLed::RedLed(gpio_num_t gpio, int output_invert, ledc_timer_t timer_num, ledc_channel_t channel) : GpioLed(gpio, output_invert, timer_num, channel) {}

RedLed::~RedLed() {}

void RedLed::OnStateChanged()
{
    // 红灯不响应设备状态变化，因此为空实现
}

void RedLed::UpdateBatteryStatus(FogSeekPowerManager::PowerState state)
{
    switch (state)
    {
    case FogSeekPowerManager::PowerState::USB_POWER_CHARGING:
        // USB供电充电中：红灯呼吸效果
        StartFadeTask();
        break;

    case FogSeekPowerManager::PowerState::USB_POWER_DONE:
        // USB供电充电完成：红灯常亮
        TurnOn();
        break;

    case FogSeekPowerManager::PowerState::USB_POWER_NO_BATTERY:
    case FogSeekPowerManager::PowerState::BATTERY_POWER:
    case FogSeekPowerManager::PowerState::NO_POWER:
        TurnOff();
        break;

    case FogSeekPowerManager::PowerState::LOW_BATTERY:
        // 低电量状态：红灯100ms间隔连续闪烁
        SetBrightness(100);
        StartContinuousBlink(100);
        break;

    default:
        TurnOff();
        break;
    }

    ESP_LOGD(TAG, "Red LED updated for power state: %d", static_cast<int>(state));
}

// ==================== GreenLed Implementation ====================
GreenLed::GreenLed(gpio_num_t gpio, int output_invert, ledc_timer_t timer_num, ledc_channel_t channel) : GpioLed(gpio, output_invert, timer_num, channel) {}
GreenLed::~GreenLed() {}

void GreenLed::OnStateChanged()
{
    if (ignore_state_changes_)
    {
        TurnOff();
        return;
    }

    auto &app = Application::GetInstance();
    auto device_state = app.GetDeviceState();

    switch (device_state)
    {
    case kDeviceStateIdle: // 空闲状态：绿灯呼吸效果
        StartFadeTask();
        break;

    case kDeviceStateListening: // 监听状态：绿灯常亮
        TurnOn();
        break;

    case kDeviceStateSpeaking: // 说话状态：绿灯1000ms间隔连续闪烁
        StartContinuousBlink(800);
        break;

    case kDeviceStateStarting:        // 启动状态
    case kDeviceStateWifiConfiguring: // WiFi配置状态
    case kDeviceStateConnecting:      // 连接状态的处理
    case kDeviceStateUpgrading:       // 升级状态
    case kDeviceStateActivating:      // 激活状态
    case kDeviceStateAudioTesting:    // 音频测试状态
        StartContinuousBlink(200);
        break;

    case kDeviceStateFatalError: // 致命错误状态
        StartContinuousBlink(100);
        break;

    case kDeviceStateUnknown: // 未知状态的处理
        TurnOff();
        break;

    default:
        ESP_LOGE(TAG, "Unknown device state: %d", static_cast<int>(device_state));
        return;
    }

    ESP_LOGD(TAG, "Green LED updated for device state: %d", static_cast<int>(device_state));
}

// ==================== RgbStrip Implementation ====================
RgbLedStrip::RgbLedStrip(gpio_num_t gpio, uint8_t num_leds)
    : CircularStrip(gpio, num_leds), num_leds_(num_leds)
{
    ESP_LOGI("RgbLedStrip", "Initialized with %d LEDs", num_leds);
}

void RgbLedStrip::SetAllColor(StripColor color)
{
    // 更新当前颜色
    current_color_ = color;

    CircularStrip::SetAllColor(color);
}

void RgbLedStrip::SetSingleColor(uint8_t index, StripColor color)
{
    // 更新当前颜色（仅当设置第一个LED时）
    if (index == 0)
    {
        current_color_ = color;
    }
    CircularStrip::SetSingleColor(index, color);
}

void RgbLedStrip::Blink(StripColor color, int interval_ms)
{
    current_color_ = color;
    CircularStrip::Blink(color, interval_ms);
}

void RgbLedStrip::Scroll(StripColor low, StripColor high, int length, int interval_ms)
{
    // 记录当前颜色（使用高亮色作为主要颜色）
    current_color_ = high;
    CircularStrip::Scroll(low, high, length, interval_ms);
}

void RgbLedStrip::StartBreathe(int breath_time_ms)
{
    // 获取当前颜色
    StripColor current_color = current_color_;

    // 计算每个颜色等分的步长（将当前颜色分为 50 个等分）
    StripColor step_size;
    step_size.red = current_color.red / 50;
    step_size.green = current_color.green / 50;
    step_size.blue = current_color.blue / 50;

    // 计算每个时间段的毫秒数（呼吸时间分成 100 个等分，增强和减弱各占一半）
    int time_per_step = (breath_time_ms / 2) / 50;
    if (time_per_step < 10)
    {
        time_per_step = 10; // 最小时间间隔限制
    }

    // 用于记录当前状态
    int step_index = 0;
    bool increasing = true; // true 表示增强，false 表示减弱

    StartStripTask(time_per_step, [this, current_color, step_size, step_index, increasing]() mutable
                   {
        StripColor display_color;
        
        if (increasing) {
            // 正向呼吸：逐渐增强颜色（从 0 到当前颜色）
            display_color.red = step_size.red * step_index;
            display_color.green = step_size.green * step_index;
            display_color.blue = step_size.blue * step_index;
            
            if (step_index >= 50) {
                increasing = false;
                step_index = 50;
            } else {
                step_index++;
            }
        } else {
            // 反向呼吸：逐渐减弱颜色（从当前颜色到 0）
            display_color.red = step_size.red * step_index;
            display_color.green = step_size.green * step_index;
            display_color.blue = step_size.blue * step_index;
            
            if (step_index <= 0) {
                increasing = true;
                step_index = 0;
            } else {
                step_index--;
            }
        }
        
        // 设置所有 LED 为当前计算出的颜色
        for (int i = 0; i < num_leds_; i++)
        {
            led_strip_set_pixel(led_strip_, i, display_color.red, display_color.green, display_color.blue);
        }
        led_strip_refresh(led_strip_); });
}

void RgbLedStrip::TurnOnStrip(int total_time_ms, StripColor color)
{
    // 先关闭所有 LED，确保从全黑状态开始
    SetAllColor(StripColor{0, 0, 0});

    // 计算每个 LED 的时间间隔
    int time_per_led = total_time_ms / num_leds_;
    if (time_per_led < 50)
    {
        time_per_led = 50; // 最小时间间隔限制
    }

    ESP_LOGI("RgbLedStrip", "PowerOn Sequence: %d LEDs, %d ms total, %d ms per LED",
             num_leds_, total_time_ms, time_per_led);

    // 用于记录当前点亮的 LED 索引
    int current_led = 0;

    StartStripTask(time_per_led, [this, color, current_led]() mutable
                   {
                       if (current_led >= num_leds_)
                       {
                           // 所有 LED 已点亮，停止定时器
                           esp_timer_stop(strip_timer_);
                           
                           // 记录最终颜色
                           current_color_ = color;
                           return;
                       }

                       // 设置第 current_led 个 LED 为目标颜色
                       led_strip_set_pixel(led_strip_, current_led, color.red, color.green, color.blue);
                       led_strip_refresh(led_strip_);

                       ESP_LOGD("RgbLedStrip", "LED %d lit with color (%d, %d, %d)",
                                current_led, color.red, color.green, color.blue);

                       current_led++; });
}

void RgbLedStrip::TurnOffStrip(int fade_time_ms)
{
    // 获取当前颜色
    StripColor current_color = current_color_;

    // 计算每个颜色等分的步长（将当前颜色分为 50 个等分）
    StripColor step_size;
    step_size.red = current_color.red / 50;
    step_size.green = current_color.green / 50;
    step_size.blue = current_color.blue / 50;

    // 计算每个时间段的毫秒数（熄灯时间分成 50 个等分）
    int time_per_step = fade_time_ms / 50;
    if (time_per_step < 10)
    {
        time_per_step = 10; // 最小时间间隔限制
    }

    // 用于记录当前状态
    int step_index = 50; // 从最大值开始递减

    StartStripTask(time_per_step, [this, current_color, step_size, step_index]() mutable
                   {
        StripColor display_color;
        
        // 逐渐减弱颜色（从当前颜色到 0）
        display_color.red = step_size.red * step_index;
        display_color.green = step_size.green * step_index;
        display_color.blue = step_size.blue * step_index;
        
        if (step_index < 0) {
            // 完全熄灭，但不更新 current_color_，保持原始颜色
            led_strip_clear(led_strip_);
            return;
        } else {
            step_index--;
        }
        
        // 设置所有 LED 为当前计算出的颜色
        for (int i = 0; i < num_leds_; i++)
        {
            led_strip_set_pixel(led_strip_, i, display_color.red, display_color.green, display_color.blue);
        }
        led_strip_refresh(led_strip_); });
}

void RgbLedStrip::IncreaseBrightness()
{
    if (brightness_level_ < 5)
    {
        brightness_level_++;
        ApplyBrightness();
        ESP_LOGI(TAG, "Brightness increased to level %d", brightness_level_);
    }
}

void RgbLedStrip::DecreaseBrightness()
{
    if (brightness_level_ > 0)
    {
        brightness_level_--;
        ApplyBrightness();
        ESP_LOGI(TAG, "Brightness decreased to level %d", brightness_level_);
    }
}

void RgbLedStrip::ApplyBrightness()
{
    // 计算亮度比例 (0, 0.2, 0.4, 0.6, 0.8, 1.0)
    float brightness_ratio = brightness_level_ / 5.0f;

    // 按比例调整 RGB 值
    StripColor adjusted_color;
    adjusted_color.red = (uint8_t)(current_color_.red * brightness_ratio);
    adjusted_color.green = (uint8_t)(current_color_.green * brightness_ratio);
    adjusted_color.blue = (uint8_t)(current_color_.blue * brightness_ratio);
    ESP_LOGI(TAG, "Apply brightness level %d: ratio=%.2f, color=(%d, %d, %d)",
             brightness_level_, brightness_ratio,
             adjusted_color.red, adjusted_color.green, adjusted_color.blue);

    // 直接调用父类方法设置颜色
    CircularStrip::SetAllColor(adjusted_color);
}

// ==================== FogSeekLedController Implementation ====================

/**
 * @brief 构造函数 - 初始化 LED控制器
 */
FogSeekLedController::FogSeekLedController() : pin_config_()
{
}

/**
 * @brief 析构函数 - 清理资源
 */
FogSeekLedController::~FogSeekLedController()
{
    // 删除红灯控制器
    if (red_led_)
    {
        delete red_led_;
        red_led_ = nullptr;
    }

    // 删除绿灯控制器
    if (green_led_)
    {
        delete green_led_;
        green_led_ = nullptr;
    }

    // 删除冷暖色灯控制器
    if (cold_light_)
    {
        delete cold_light_;
        cold_light_ = nullptr;
    }

    if (warm_light_)
    {
        delete warm_light_;
        warm_light_ = nullptr;
    }

    // 删除 RGB 灯带控制器
    if (rgb_led_strip_)
    {
        delete rgb_led_strip_;
        rgb_led_strip_ = nullptr;
    }
}

/**
 * @brief 初始化LED GPIO
 *
 * @param power_manager 电源管理器引用
 * @param pin_config LED 引脚配置
 */
void FogSeekLedController::InitializeLeds(FogSeekPowerManager &power_manager, const led_pin_config_t *pin_config)
{
    // 保存引脚配置
    pin_config_ = *pin_config;

    // 初始化红灯
    if (pin_config->red_gpio >= 0)
    {
        // 为红灯和绿灯分配不同的 LEDC 通道，避免冲突
        red_led_ = new RedLed(static_cast<gpio_num_t>(pin_config->red_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_1);
    }

    // 初始化绿灯
    if (pin_config->green_gpio >= 0)
    {
        // 为红灯和绿灯分配不同的 LEDC 通道，避免冲突
        green_led_ = new GreenLed(static_cast<gpio_num_t>(pin_config->green_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_2);
    }

    // 如果配置了冷暖色灯 GPIO，则初始化冷暖色灯
    if (pin_config->cold_light_gpio >= 0 || pin_config->warm_light_gpio >= 0)
    {
        // 初始化冷暖色灯（使用 PWM 控制）
        if (pin_config->cold_light_gpio >= 0)
        {
            // 为冷色灯和暖色灯分配不同的 LEDC 通道，避免冲突
            cold_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config->cold_light_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_3);
            cold_light_->TurnOff();
        }

        if (pin_config->warm_light_gpio >= 0)
        {
            warm_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config->warm_light_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_4);
            warm_light_->TurnOff();
        }
    }

    // 如果配置了 RGB 灯带 GPIO 和数量，则初始化 RGB 灯带
    if (pin_config->rgb_gpio >= 0 && pin_config->rgb_num_leds > 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rgb_led_strip_ = new RgbLedStrip(static_cast<gpio_num_t>(pin_config->rgb_gpio),
                                         static_cast<uint8_t>(pin_config->rgb_num_leds));
        num_leds_ = static_cast<uint8_t>(pin_config->rgb_num_leds);
        ESP_LOGI(TAG, "RGB LED strip initialized with %d LEDs on GPIO %d",
                 num_leds_, pin_config->rgb_gpio);
    }

    UpdateLedStatus(power_manager);
    ESP_LOGI(TAG, "LEDs initialized");
}

/**
 * @brief 统一更新所有LED 状态
 *
 * @param power_manager 电源管理器引用
 */
void FogSeekLedController::UpdateLedStatus(FogSeekPowerManager &power_manager)
{
    auto device_power_state = power_manager.GetDevicePowerState();
    auto power_state = power_manager.GetPowerState();

    switch (device_power_state)
    {
    case FogSeekPowerManager::DevicePowerState::CHARGING: // 充电状态，绿灯熄灭，红灯亮度正常，状态根据电源充电状态刷新
        red_led_->SetBrightness(100);
        red_led_->UpdateBatteryStatus(power_state);
        green_led_->TurnOff();
        green_led_->SetIgnoreStateChanges(true); // 设置绿灯忽略状态变化
        break;

    case FogSeekPowerManager::DevicePowerState::POWER_ON: // 开机状态，两个灯都工作（红灯亮度调低，绿灯正常）
        red_led_->SetBrightness(10);
        red_led_->UpdateBatteryStatus(power_state);
        green_led_->SetBrightness(100);
        green_led_->SetIgnoreStateChanges(false); // 恢复绿灯响应状态变化
        green_led_->OnStateChanged();
        break;

    case FogSeekPowerManager::DevicePowerState::POWER_OFF: // 关机状态，两个灯都熄灭
        red_led_->TurnOff();
        green_led_->TurnOff();
        green_led_->SetIgnoreStateChanges(true); // 设置绿灯忽略状态变化
        break;
    default:
        break;
    }
}

/**
 * @brief 控制冷色灯
 *
 * @param state true 为开启，false 为关闭
 */
void FogSeekLedController::SetColdLight(bool state)
{
    if (cold_light_)
    {
        if (state)
        {
            cold_light_->TurnOn();
            cold_light_state_ = true;
        }
        else
        {
            cold_light_->TurnOff();
            cold_light_state_ = false;
        }
    }
}

/**
 * @brief 控制暖色灯
 *
 * @param state true 为开启，false 为关闭
 */
void FogSeekLedController::SetWarmLight(bool state)
{
    if (warm_light_)
    {
        if (state)
        {
            warm_light_->TurnOn();
            warm_light_state_ = true;
        }
        else
        {
            warm_light_->TurnOff();
            warm_light_state_ = false;
        }
    }
}

/**
 * @brief 设置冷色灯亮度
 *
 * @param brightness 亮度值 (0-100)
 */
void FogSeekLedController::SetColdLightBrightness(int brightness)
{
    if (cold_light_)
    {
        cold_light_->SetBrightness(brightness);
        cold_light_state_ = (brightness > 0);
        if (brightness > 0)
        {
            cold_light_->TurnOn();
        }
        else
        {
            cold_light_->TurnOff();
        }
    }
}

/**
 * @brief 设置暖色灯亮度
 *
 * @param brightness 亮度值 (0-100)
 */
void FogSeekLedController::SetWarmLightBrightness(int brightness)
{
    if (warm_light_)
    {
        warm_light_->SetBrightness(brightness);
        warm_light_state_ = (brightness > 0);
        if (brightness > 0)
        {
            warm_light_->TurnOn();
        }
        else
        {
            warm_light_->TurnOff();
        }
    }
}

void FogSeekLedController::RunMarqueeLights(int duration_ms)
{
    StripColor current_color = rgb_led_strip_->GetCurrentColor();
    rgb_led_strip_->Scroll({0, 0, 0}, current_color, 1, 100);
}

void FogSeekLedController::TurnOffRgbLights(int duration_ms)
{
    //RgbLedStrip::TurnOffStrip(duration_ms);
    rgb_led_strip_->TurnOffStrip(duration_ms);
}
void FogSeekLedController::StartBreathingEffect(int duration_ms)
{
    rgb_led_strip_->StartBreathe(duration_ms);
}
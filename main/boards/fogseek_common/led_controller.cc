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

// 静态回调函数，转发到成员函数
static void EffectTimerCallbackStatic(void *arg)
{
    FogSeekLedController *controller = static_cast<FogSeekLedController *>(arg);
    controller->EffectTimerCallback();
}

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
    led_cache_.resize(num_leds_, {0, 0, 0});
    ESP_LOGI("RgbLedStrip", "Initialized with %d LEDs", num_leds_);
}

void RgbLedStrip::SetColor(uint8_t r, uint8_t g, uint8_t b)
{
    current_color_ = {r, g, b};
    StripColor color = {r, g, b};
    SetAllColor(color);
    std::fill(led_cache_.begin(), led_cache_.end(), color);
}

void RgbLedStrip::SetSingle(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= num_leds_)
        return;
    StripColor color = {r, g, b};
    led_cache_[index] = color;
    SetSingleColor(index, color);
}

void RgbLedStrip::SetMultiple(const std::vector<std::tuple<uint8_t, uint8_t, uint8_t, uint8_t>> &configs)
{
    for (const auto &config : configs)
    {
        uint8_t index = std::get<0>(config);
        uint8_t r = std::get<1>(config);
        uint8_t g = std::get<2>(config);
        uint8_t b = std::get<3>(config);
        SetSingle(index, r, g, b);
    }
}

void RgbLedStrip::SetBackground(uint8_t r, uint8_t g, uint8_t b)
{
    background_color_ = {r, g, b};
}

void RgbLedStrip::ResetToBackground()
{
    SetAllColor(background_color_);
    current_color_ = background_color_;
    std::fill(led_cache_.begin(), led_cache_.end(), background_color_);
}

// ==================== FogSeekLedController Implementation ====================

/**
 * @brief 构造函数 - 初始化LED控制器
 */
FogSeekLedController::FogSeekLedController() : effect_timer_(nullptr),
                                               pin_config_()
{
    // 创建通用效果定时器
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = EffectTimerCallbackStatic;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "rgb_effect_timer";
    esp_err_t err = esp_timer_create(&timer_args, &effect_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create effect timer: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 析构函数 - 清理资源
 */
FogSeekLedController::~FogSeekLedController()
{
    // 停止并删除通用效果定时器
    if (effect_timer_)
    {
        esp_timer_stop(effect_timer_);
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
    }

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
}

/**
 * @brief 初始化LED GPIO
 *
 * @param power_manager 电源管理器引用
 * @param pin_config LED引脚配置
 */
void FogSeekLedController::InitializeLeds(FogSeekPowerManager &power_manager, const led_pin_config_t *pin_config)
{
    // 保存引脚配置
    pin_config_ = *pin_config;

    // 初始化红灯
    if (pin_config->red_gpio >= 0)
    {
        // 为红灯和绿灯分配不同的LEDC通道，避免冲突
        red_led_ = new RedLed(static_cast<gpio_num_t>(pin_config->red_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_1);
    }

    // 初始化绿灯
    if (pin_config->green_gpio >= 0)
    {
        // 为红灯和绿灯分配不同的LEDC通道，避免冲突
        green_led_ = new GreenLed(static_cast<gpio_num_t>(pin_config->green_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_2);
    }

    // 如果配置了冷暖色灯GPIO，则初始化冷暖色灯
    if (pin_config->cold_light_gpio >= 0 || pin_config->warm_light_gpio >= 0)
    {
        // 初始化冷暖色灯（使用PWM控制）
        if (pin_config->cold_light_gpio >= 0)
        {
            // 为冷色灯和暖色灯分配不同的LEDC通道，避免冲突
            cold_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config->cold_light_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_3);
            cold_light_->TurnOff();
        }

        if (pin_config->warm_light_gpio >= 0)
        {
            warm_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config->warm_light_gpio), 0, LEDC_TIMER_1, LEDC_CHANNEL_4);
            warm_light_->TurnOff();
        }
    }

    UpdateLedStatus(power_manager);
    ESP_LOGI(TAG, "LEDs initialized");
}

/**
 * @brief 统一更新所有LED状态
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
 * @param state true为开启，false为关闭
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
 * @param state true为开启，false为关闭
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

/**
 * @brief 设置RGB灯带实例
 *
 * @param strip RGB灯带实例指针
 * @param num_leds 灯珠数量
 */
void FogSeekLedController::SetRgbStrip(CircularStrip *strip, uint8_t num_leds)
{
    rgb_led_strip_ = strip;
    rgb_num_leds_ = num_leds;

    // 初始化默认颜色，防止original_color_为全黑
    if (original_color_.red == 0 && original_color_.green == 0 && original_color_.blue == 0)
    {
        original_color_ = {100, 100, 100}; // 设置默认为淡白色
        current_color_ = {0, 0, 0};        // 初始状态为关闭
    }

    ESP_LOGI(TAG, "RGB strip set with %d LEDs", num_leds);
}

// 实际的通用效果定时器回调函数
void FogSeekLedController::EffectTimerCallback()
{
    if (!is_effect_running_ || !rgb_led_strip_)
    {
        return;
    }

    bool continue_effect = false;

    switch (current_effect_type_)
    {
    case MARQUEE_EFFECT:
    {
        if (effect_current_step_ < rgb_num_leds_)
        {
            // 使用当前亮度等级来控制跑马灯颜色
            uint8_t brightness_factor = brightness_levels_[current_brightness_level_];
            StripColor color = {
                .red = static_cast<uint8_t>((original_color_.red * brightness_factor) / 100),
                .green = static_cast<uint8_t>((original_color_.green * brightness_factor) / 100),
                .blue = static_cast<uint8_t>((original_color_.blue * brightness_factor) / 100)};

            // 先清除所有LED，然后只点亮当前的LED
            if (effect_current_step_ == 0)
            {
                // 第一次时清除所有LED
                StripColor off_color = {0, 0, 0};
                rgb_led_strip_->SetAllColor(off_color);
            }

            rgb_led_strip_->SetSingleColor(effect_current_step_, color);

            effect_current_step_++;
            continue_effect = true;
        }
        else
        {
            // 效果完成，将当前颜色设置为与亮度匹配的颜色
            uint8_t brightness_factor = brightness_levels_[current_brightness_level_];
            current_color_ = {
                .red = static_cast<uint8_t>((original_color_.red * brightness_factor) / 100),
                .green = static_cast<uint8_t>((original_color_.green * brightness_factor) / 100),
                .blue = static_cast<uint8_t>((original_color_.blue * brightness_factor) / 100)};
            original_color_ = current_color_;

            ESP_LOGI(TAG, "Marquee lights effect completed");
            is_effect_running_ = false;
        }
        break;
    }

    case TURN_ON_LIGHTS:
    {
        if (effect_current_step_ <= effect_total_steps_)
        {
            uint8_t brightness = (brightness_levels_[current_brightness_level_] * effect_current_step_) / effect_total_steps_;
            uint8_t adjusted_red = static_cast<uint8_t>((original_color_.red * brightness) / 100);
            uint8_t adjusted_green = static_cast<uint8_t>((original_color_.green * brightness) / 100);
            uint8_t adjusted_blue = static_cast<uint8_t>((original_color_.blue * brightness) / 100);
            StripColor color = {
                .red = adjusted_red,
                .green = adjusted_green,
                .blue = adjusted_blue};
            rgb_led_strip_->SetAllColor(color);
            current_color_ = color;

            effect_current_step_++;
            continue_effect = true;
        }
        else
        {
            // 更新当前颜色以反映最终亮度
            uint8_t target_brightness = brightness_levels_[current_brightness_level_];
            uint8_t target_red = static_cast<uint8_t>((original_color_.red * target_brightness) / 100);
            uint8_t target_green = static_cast<uint8_t>((original_color_.green * target_brightness) / 100);
            uint8_t target_blue = static_cast<uint8_t>((original_color_.blue * target_brightness) / 100);
            current_color_ = {target_red, target_green, target_blue};

            is_effect_running_ = false;
        }
        break;
    }

    case TURN_OFF_LIGHTS:
    {
        if (effect_current_step_ <= effect_total_steps_)
        {
            uint8_t start_brightness = brightness_levels_[current_brightness_level_];
            uint8_t brightness = start_brightness - (start_brightness * effect_current_step_) / effect_total_steps_;
            uint8_t adjusted_red = (original_color_.red * brightness) / 100;
            uint8_t adjusted_green = (original_color_.green * brightness) / 100;
            uint8_t adjusted_blue = (original_color_.blue * brightness) / 100;
            StripColor color = {
                .red = adjusted_red,
                .green = adjusted_green,
                .blue = adjusted_blue};

            rgb_led_strip_->SetAllColor(color);

            effect_current_step_++;
            continue_effect = true;
        }
        else
        {
            // 最终完全关闭
            StripColor off_color = {0, 0, 0};
            rgb_led_strip_->SetAllColor(off_color);

            ESP_LOGI(TAG, "RGB lights turned off");

            is_effect_running_ = false;
        }
        break;
    }

    case BREATHING_EFFECT:
    {
        // 使用正弦波函数创建平滑的呼吸效果
        float angle = (2.0f * M_PI * effect_current_step_) / effect_total_steps_;
        float brightness_ratio = (sin(angle) + 1.0f) / 2.0f; // 映射到0.0-1.0范围

        // 根据原始颜色和当前亮度系数计算实际颜色
        uint8_t adjusted_red = static_cast<uint8_t>(original_color_.red * brightness_ratio);
        uint8_t adjusted_green = static_cast<uint8_t>(original_color_.green * brightness_ratio);
        uint8_t adjusted_blue = static_cast<uint8_t>(original_color_.blue * brightness_ratio);

        StripColor color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};

        // 设置所有LED的颜色
        rgb_led_strip_->SetAllColor(color);

        // 更新步骤，循环
        effect_current_step_ = (effect_current_step_ + 1) % effect_total_steps_;
        continue_effect = true; // 呼吸效果是持续的，除非被停止
        break;
    }
    default:
        is_effect_running_ = false;
        break;
    }

    // 如果需要继续效果，重新启动定时器
    if (continue_effect && is_effect_running_)
    {
        esp_timer_start_once(effect_timer_, effect_delay_per_step_ * 1000);
    }
}

// 启动通用效果
bool FogSeekLedController::StartEffect(EffectType type, int duration_ms)
{
    if (!rgb_led_strip_ || is_effect_running_)
    {
        ESP_LOGW(TAG, "Cannot start effect: strip not initialized or effect already running");
        return false;
    }

    // 设置效果参数
    current_effect_type_ = type;
    effect_duration_ms_ = duration_ms;

    switch (type)
    {
    case MARQUEE_EFFECT:
        effect_total_steps_ = rgb_num_leds_;
        effect_current_step_ = 0;
        effect_delay_per_step_ = duration_ms / rgb_num_leds_;
        break;
    case TURN_ON_LIGHTS:
    case TURN_OFF_LIGHTS:
        effect_total_steps_ = 20; // 分成20步完成过渡
        effect_current_step_ = 0;
        effect_delay_per_step_ = duration_ms / effect_total_steps_;
        break;
    case BREATHING_EFFECT:
        effect_total_steps_ = 80; // 一个周期的步数
        effect_current_step_ = 0;
        effect_delay_per_step_ = duration_ms / effect_total_steps_;
        break;
    case NONE:
        return false;
    }

    is_effect_running_ = true;

    // 启动定时器
    esp_err_t err = esp_timer_start_once(effect_timer_, effect_delay_per_step_ * 1000);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start effect timer: %s", esp_err_to_name(err));
        is_effect_running_ = false;
        return false;
    }
    ESP_LOGI(TAG, "Effect started: type %d, duration %d ms", type, duration_ms);
    return true;
}

// 停止当前效果
void FogSeekLedController::StopCurrentEffect()
{
    if (effect_timer_)
    {
        esp_timer_stop(effect_timer_);
    }
    is_effect_running_ = false;
    current_effect_type_ = NONE;
}

// 跑马灯效果，依次点亮所有灯
bool FogSeekLedController::RunMarqueeLights(int total_time_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return false;
    }

    return StartEffect(MARQUEE_EFFECT, total_time_ms);
}

// 打开灯光
bool FogSeekLedController::TurnOnRgbLights(int duration_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return false;
    }

    return StartEffect(TURN_ON_LIGHTS, duration_ms);
}

// 关闭灯光
bool FogSeekLedController::TurnOffRgbLights(int duration_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return false;
    }

    return StartEffect(TURN_OFF_LIGHTS, duration_ms);
}

// 开始呼吸效果
bool FogSeekLedController::StartBreathingEffect(int cycle_duration_ms)
{
    if (!rgb_led_strip_)
    {
        ESP_LOGE(TAG, "RGB strip not initialized");
        return false;
    }

    // 如果已有其他效果在运行，先停止
    StopCurrentEffect();

    return StartEffect(BREATHING_EFFECT, cycle_duration_ms);
}

// 停止呼吸效果
void FogSeekLedController::StopBreathingEffect()
{
    if (current_effect_type_ == BREATHING_EFFECT)
    {
        StopCurrentEffect();

        // 恢复到当前设置的颜色和亮度
        uint8_t target_brightness = brightness_levels_[current_brightness_level_];
        uint8_t adjusted_red = (original_color_.red * target_brightness) / 100;
        uint8_t adjusted_green = (original_color_.green * target_brightness) / 100;
        uint8_t adjusted_blue = (original_color_.blue * target_brightness) / 100;
        StripColor color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};
        rgb_led_strip_->SetAllColor(color);
        current_color_ = color;
    }
}

/**
 * @brief 增加RGB灯带亮度一个档位
 */
void FogSeekLedController::IncreaseBrightness()
{
    if (current_brightness_level_ < 4)
    {
        current_brightness_level_++;
        ESP_LOGI(TAG, "Brightness increased to level %d (%d%%)",
                 current_brightness_level_, brightness_levels_[current_brightness_level_]);

        // 使用原始颜色和新的亮度级别来计算当前颜色
        uint8_t target_brightness = brightness_levels_[current_brightness_level_];
        uint8_t adjusted_red = (original_color_.red * target_brightness) / 100;
        uint8_t adjusted_green = (original_color_.green * target_brightness) / 100;
        uint8_t adjusted_blue = (original_color_.blue * target_brightness) / 100;
        StripColor new_color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};
        rgb_led_strip_->SetAllColor(new_color);

        // 更新当前颜色为新颜色
        current_color_ = new_color;

        ESP_LOGD(TAG, "RGB values after brightness increase: R=%d, G=%d, B=%d",
                 new_color.red, new_color.green, new_color.blue);
    }
    else
    {
        ESP_LOGI(TAG, "Brightness already at maximum level");
    }
}

/**
 * @brief 降低RGB灯带亮度一个档位
 */
void FogSeekLedController::DecreaseBrightness()
{
    if (current_brightness_level_ > 0)
    {
        current_brightness_level_--;
        ESP_LOGI(TAG, "Brightness decreased to level %d (%d%%)",
                 current_brightness_level_, brightness_levels_[current_brightness_level_]);

        // 使用原始颜色和新的亮度级别来计算当前颜色
        uint8_t target_brightness = brightness_levels_[current_brightness_level_];
        uint8_t adjusted_red = (original_color_.red * target_brightness) / 100;
        uint8_t adjusted_green = (original_color_.green * target_brightness) / 100;
        uint8_t adjusted_blue = (original_color_.blue * target_brightness) / 100;
        StripColor new_color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};
        rgb_led_strip_->SetAllColor(new_color);

        // 更新当前颜色为新颜色
        current_color_ = new_color;

        ESP_LOGD(TAG, "RGB values after brightness decrease: R=%d, G=%d, B=%d",
                 new_color.red, new_color.green, new_color.blue);
    }
    else
    {
        ESP_LOGI(TAG, "Brightness already at minimum level");
    }
}

/**
 * @brief RGB灯带颜色更换（红橙黄绿青蓝紫）
 */
void FogSeekLedController::ChangeToRandomColors()
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return;
    }

    // 定义彩虹颜色（红橙黄绿青蓝紫）
    StripColor rainbow_colors[] = {
        {255, 0, 0},   // 红色
        {255, 165, 0}, // 橙色
        {255, 255, 0}, // 黄色
        {0, 255, 0},   // 绿色
        {0, 127, 255}, // 青色
        {0, 0, 255},   // 蓝色
        {139, 0, 255}  // 紫色
    };
    int num_colors = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

    // 按顺序选择下一个颜色
    static int color_index = 0; // 静态变量用于记住上一次的颜色索引
    StripColor selected_color = rainbow_colors[color_index % num_colors];

    // 更新颜色索引以供下次调用
    color_index++;

    // 保存原始颜色（未经过亮度调整的纯色）
    original_color_ = selected_color;

    // 根据当前亮度级别调整颜色强度
    uint8_t brightness_factor = brightness_levels_[current_brightness_level_];
    selected_color.red = (selected_color.red * brightness_factor) / 100;
    selected_color.green = (selected_color.green * brightness_factor) / 100;
    selected_color.blue = (selected_color.blue * brightness_factor) / 100;

    // 将选中的颜色设置给所有灯珠
    rgb_led_strip_->SetAllColor(selected_color);

    // 更新当前颜色
    current_color_ = selected_color;

    ESP_LOGI(TAG, "RGB lights changed to rainbow color, RGB: R=%d, G=%d, B=%d",
             current_color_.red, current_color_.green, current_color_.blue);
}

/**
 * @brief 设置RGB灯带自定义颜色
 */
void FogSeekLedController::SetCustomColor(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return;
    }

    // 保存原始颜色（未经过亮度调整的纯色）
    original_color_ = {red, green, blue};

    // 根据当前亮度级别调整颜色强度
    uint8_t brightness_factor = brightness_levels_[current_brightness_level_];
    StripColor adjusted_color = {
        .red = static_cast<uint8_t>((red * brightness_factor) / 100),
        .green = static_cast<uint8_t>((green * brightness_factor) / 100),
        .blue = static_cast<uint8_t>((blue * brightness_factor) / 100)};

    // 将自定义颜色设置给所有灯珠
    rgb_led_strip_->SetAllColor(adjusted_color);

    // 更新当前颜色
    current_color_ = adjusted_color;

    ESP_LOGI(TAG, "RGB lights set to custom color, RGB: R=%d, G=%d, B=%d",
             current_color_.red, current_color_.green, current_color_.blue);
}

/**
 * @brief 设置单个LED的颜色
 */
void FogSeekLedController::SetSingleLedColor(uint8_t led_index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return;
    }

    if (led_index >= rgb_num_leds_)
    {
        ESP_LOGW(TAG, "LED index %d is out of range (0-%d)", led_index, rgb_num_leds_ - 1);
        return;
    }

    // 创建并设置单个LED的颜色
    StripColor color = {red, green, blue};
    rgb_led_strip_->SetSingleColor(led_index, color);

    ESP_LOGI(TAG, "LED %d set to custom color, RGB: R=%d, G=%d, B=%d",
             led_index, red, green, blue);
}

/**
 * @brief 设置RGB LED彩虹效果，每个LED显示不同颜色
 */
void FogSeekLedController::SetRainbowColor()
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return;
    }

    // 定义彩虹颜色数组（红、橙、黄、绿、青、蓝、紫）
    StripColor rainbow_colors[] = {
        {255, 0, 0},   // 红色
        {255, 165, 0}, // 橙色
        {255, 255, 0}, // 黄色
        {0, 255, 0},   // 绿色
        {0, 127, 255}, // 青色
        {0, 0, 255},   // 蓝色
        {139, 0, 255}  // 紫色
    };

    int num_colors = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

    // 为每个LED设置不同颜色
    for (int i = 0; i < rgb_num_leds_; i++)
    {
        StripColor color = rainbow_colors[i % num_colors];
        rgb_led_strip_->SetSingleColor(i, color);
    }

    ESP_LOGI(TAG, "Rainbow color effect set for all %d LEDs", rgb_num_leds_);
}

/**
 * @brief 设置多个LED的不同颜色
 */
void FogSeekLedController::SetMultipleLedColors(const std::vector<std::tuple<uint8_t, uint8_t, uint8_t, uint8_t>> &led_colors)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
    {
        ESP_LOGW(TAG, "RGB strip not initialized or has 0 LEDs");
        return;
    }

    // 遍历传入的颜色向量，为每个指定的LED设置颜色
    for (const auto &color_data : led_colors)
    {
        uint8_t led_index = std::get<0>(color_data);
        uint8_t red = std::get<1>(color_data);
        uint8_t green = std::get<2>(color_data);
        uint8_t blue = std::get<3>(color_data);

        if (led_index >= rgb_num_leds_)
        {
            ESP_LOGW(TAG, "LED index %d is out of range (0-%d)", led_index, rgb_num_leds_ - 1);
            continue; // 跳过无效的LED索引
        }

        // 设置指定LED的颜色
        StripColor color = {red, green, blue};
        rgb_led_strip_->SetSingleColor(led_index, color);

        ESP_LOGD(TAG, "LED %d set to color - R: %d, G: %d, B: %d", led_index, red, green, blue);
    }

    ESP_LOGI(TAG, "%zu individual LED colors set", led_colors.size());
}

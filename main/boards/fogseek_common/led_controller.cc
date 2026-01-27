#include "led_controller.h"
#include "power_manager.h"
#include "../../application.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <memory>
#include <algorithm>
#include <random>

/// 日志标签
const char *FogSeekLedController::TAG = "FogSeekLedController";
const char *RedLed::TAG = "RedLed";
const char *GreenLed::TAG = "GreenLed";

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
        // USB供电无电池：红灯熄灭
        TurnOff();
        break;

    case FogSeekPowerManager::PowerState::BATTERY_POWER:
        // 电池供电：红灯熄灭
        TurnOff();
        break;

    case FogSeekPowerManager::PowerState::LOW_BATTERY:
        // 低电量状态：红灯100ms间隔连续闪烁
        SetBrightness(100);
        StartContinuousBlink(100);
        break;

    case FogSeekPowerManager::PowerState::NO_POWER:
        // 无电源：红灯熄灭
        TurnOff();
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
        ESP_LOGE(TAG, "Unknown gpio led event: %d", static_cast<int>(device_state));
        return;
    }

    ESP_LOGD(TAG, "Green LED updated for device state: %d", static_cast<int>(device_state));
}

// ==================== FogSeekLedController Implementation ====================

/**
 * @brief 构造函数 - 初始化LED控制器
 */
FogSeekLedController::FogSeekLedController() : red_led_state_(false),
                                               green_led_state_(false),
                                               red_led_(nullptr),
                                               green_led_(nullptr),
                                               cold_light_(nullptr),
                                               warm_light_(nullptr),
                                               cold_light_state_(false),
                                               warm_light_state_(false),
                                               rgb_led_strip_(nullptr),
                                               rgb_num_leds_(0),
                                               current_brightness_level_(2),
                                               original_color_({255, 255, 255}) // 初始化原始颜色为白色
{
}

/**
 * @brief 析构函数 - 清理资源
 */
FogSeekLedController::~FogSeekLedController()
{
    // 注意：RGB灯带由外部创建和销毁，这里不需要删除

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
    ESP_LOGI(TAG, "RGB strip set with %d LEDs", num_leds);
}

/**
 * @brief 开机序列，在指定时间内依次点亮所有灯
 *
 * @param total_time_ms 总时间（毫秒）
 */
void FogSeekLedController::PowerOnSequence(int total_time_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
        return;

    // 计算每个灯珠之间的延迟时间
    int delay_per_led = total_time_ms / rgb_num_leds_;

    // 依次点亮每个灯珠
    for (uint8_t i = 0; i < rgb_num_leds_; i++)
    {
        // 使用亮蓝色作为开机颜色
        uint8_t brightness = brightness_levels_[current_brightness_level_];
        uint8_t adjusted_red = (0 * brightness) / 100;    // 红色分量为0
        uint8_t adjusted_green = (0 * brightness) / 100;  // 绿色分量为0
        uint8_t adjusted_blue = (255 * brightness) / 100; // 蓝色分量为最大值
        StripColor color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};

        rgb_led_strip_->SetSingleColor(i, color);

        // 延迟一段时间
        vTaskDelay(pdMS_TO_TICKS(delay_per_led));
    }

    // 设置当前颜色为亮蓝色
    current_color_ = {
        .red = 0,
        .green = 0,
        .blue = 255};
    original_color_ = current_color_; // 同时设置原始颜色

    ESP_LOGI(TAG, "Power-on sequence completed");
}

/**
 * @brief 打开所有RGB灯光，从暗到亮
 *
 * @param duration_ms 持续时间（毫秒）
 */
void FogSeekLedController::TurnOnRgbLights(int duration_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
        return;

    // 确保亮度等级有效
    if (current_brightness_level_ >= 5)
        current_brightness_level_ = 4;

    // 逐渐增加亮度
    int steps = 20; // 分成20步完成过渡
    int step_delay = duration_ms / steps;
    uint8_t target_brightness = brightness_levels_[current_brightness_level_];

    // 使用原始颜色进行亮度渐变
    for (int step = 0; step <= steps; step++)
    {
        uint8_t brightness = (target_brightness * step) / steps;
        uint8_t adjusted_red = (original_color_.red * brightness) / 100; // 使用原始颜色
        uint8_t adjusted_green = (original_color_.green * brightness) / 100;
        uint8_t adjusted_blue = (original_color_.blue * brightness) / 100;
        StripColor color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};

        rgb_led_strip_->SetAllColor(color);
        current_color_ = color; // 记录当前颜色
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }

    // 更新当前颜色以反映最终亮度
    uint8_t target_red = (original_color_.red * target_brightness) / 100;
    uint8_t target_green = (original_color_.green * target_brightness) / 100;
    uint8_t target_blue = (original_color_.blue * target_brightness) / 100;
    current_color_ = {target_red, target_green, target_blue};

    ESP_LOGI(TAG, "RGB values after turning lights on: R=%d, G=%d, B=%d",
             current_color_.red, current_color_.green, current_color_.blue);
}

/**
 * @brief 关闭所有RGB灯光，从亮到暗
 *
 * @param duration_ms 持续时间（毫秒）
 */
void FogSeekLedController::TurnOffRgbLights(int duration_ms)
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
        return;

    // 逐渐减少亮度
    int steps = 20; // 分成20步完成过渡
    int step_delay = duration_ms / steps;
    uint8_t start_brightness = brightness_levels_[current_brightness_level_];

    for (int step = 0; step <= steps; step++)
    {
        uint8_t brightness = start_brightness - (start_brightness * step) / steps;
        uint8_t adjusted_red = (original_color_.red * brightness) / 100; // 使用原始颜色
        uint8_t adjusted_green = (original_color_.green * brightness) / 100;
        uint8_t adjusted_blue = (original_color_.blue * brightness) / 100;
        StripColor color = {
            .red = adjusted_red,
            .green = adjusted_green,
            .blue = adjusted_blue};

        rgb_led_strip_->SetAllColor(color);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }

    // 最终完全关闭
    StripColor off_color = {0, 0, 0};
    rgb_led_strip_->SetAllColor(off_color);

    // 保持当前颜色不变，这样重新开启时可以恢复之前颜色

    ESP_LOGI(TAG, "RGB lights turned off, final RGB values: R=%d, G=%d, B=%d",
             off_color.red, off_color.green, off_color.blue);

    ESP_LOGI(TAG, "RGB lights turned off");
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

        ESP_LOGI(TAG, "RGB values after brightness increase: R=%d, G=%d, B=%d",
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

        ESP_LOGI(TAG, "RGB values after brightness decrease: R=%d, G=%d, B=%d",
                 new_color.red, new_color.green, new_color.blue);
    }
    else
    {
        ESP_LOGI(TAG, "Brightness already at minimum level");
    }
}

/**
 * @brief 将RGB灯带颜色变为随机颜色（红橙黄绿青蓝紫）
 */
void FogSeekLedController::ChangeToRandomColors()
{
    if (!rgb_led_strip_ || rgb_num_leds_ == 0)
        return;

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

    // 使用更简单的随机数生成方式，避免使用可能阻塞的random_device
    uint32_t rand_val = esp_random(); // 使用ESP-IDF提供的随机数函数
    int random_index = rand_val % num_colors;
    StripColor selected_color = rainbow_colors[random_index];

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

    ESP_LOGI(TAG, "RGB lights changed to random rainbow colors, new RGB values: R=%d, G=%d, B=%d",
             current_color_.red, current_color_.green, current_color_.blue);
}
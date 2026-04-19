#include "tca6408a_led_controller.h"
#include "power_manager.h"
#include <esp_log.h>

/// 日志标签
const char *Tca6408aLedController::TAG = "Tca6408aLedController";
const char *Tca6408aRedLed::TAG = "Tca6408aRedLed";
const char *Tca6408aGreenLed::TAG = "Tca6408aGreenLed";

// ==================== Tca6408aRedLed Implementation ====================

Tca6408aRedLed::Tca6408aRedLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio)
    : Tca6408aLed(tca_handle, gpio)
{
}

Tca6408aRedLed::~Tca6408aRedLed()
{
}

void Tca6408aRedLed::OnStateChanged()
{
    // 红灯不响应设备状态变化，因此为空实现
}

void Tca6408aRedLed::UpdateBatteryStatus(PowerState state)
{
    switch (state)
    {
    case PowerState::USB_POWER_CHARGING:
        // USB供电充电中：红灯慢闪（1秒间隔）
        StartContinuousBlink(1000);
        break;

    case PowerState::USB_POWER_DONE:
        // USB供电充电完成：红灯常亮
        TurnOn();
        break;

    case PowerState::USB_POWER_NO_BATTERY:
    case PowerState::BATTERY_POWER:
    case PowerState::NO_POWER:
        // 无电池或电池供电：红灯熄灭
        TurnOff();
        break;

    case PowerState::LOW_BATTERY:
        // 低电量状态：红灯快闪（200ms间隔）
        StartContinuousBlink(200);
        break;

    default:
        TurnOff();
        break;
    }

    ESP_LOGD(TAG, "Red LED updated for power state: %d", static_cast<int>(state));
}

// ==================== Tca6408aGreenLed Implementation ====================

Tca6408aGreenLed::Tca6408aGreenLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio)
    : Tca6408aLed(tca_handle, gpio)
{
}

Tca6408aGreenLed::~Tca6408aGreenLed()
{
}

void Tca6408aGreenLed::OnStateChanged()
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
    case kDeviceStateIdle:
        // 空闲状态：绿灯熄灭（TCA6408A 不支持呼吸效果）
        TurnOff();
        break;

    case kDeviceStateListening:
        // 监听状态：绿灯常亮
        TurnOn();
        break;

    case kDeviceStateSpeaking:
        // 说话状态：绿灯慢闪（800ms间隔）
        StartContinuousBlink(800);
        break;

    case kDeviceStateStarting:        // 启动状态
    case kDeviceStateWifiConfiguring: // WiFi配置状态
    case kDeviceStateConnecting:      // 连接状态
    case kDeviceStateUpgrading:       // 升级状态
    case kDeviceStateActivating:      // 激活状态
    case kDeviceStateAudioTesting:    // 音频测试状态
        // 这些状态：绿灯快闪（200ms间隔）
        StartContinuousBlink(200);
        break;

    case kDeviceStateFatalError:
        // 致命错误状态：绿灯极快闪（100ms间隔）
        StartContinuousBlink(100);
        break;

    case kDeviceStateUnknown:
        // 未知状态：绿灯熄灭
        TurnOff();
        break;

    default:
        ESP_LOGE(TAG, "Unknown device state: %d", static_cast<int>(device_state));
        TurnOff();
        return;
    }

    ESP_LOGD(TAG, "Green LED updated for device state: %d", static_cast<int>(device_state));
}

// ==================== Tca6408aLedController Implementation ====================

/**
 * @brief 构造函数 - 初始化 LED控制器
 */
Tca6408aLedController::Tca6408aLedController() : pin_config_()
{
}

/**
 * @brief 析构函数 - 清理资源
 */
Tca6408aLedController::~Tca6408aLedController()
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
}

/**
 * @brief 初始化 LED
 *
 * @param tca_handle TCA6408A 驱动句柄指针
 * @param pin_config LED 引脚配置
 * @param power_manager 电源管理器引用
 */
void Tca6408aLedController::InitializeLeds(tca6408a_handle_t *tca_handle, const tca6408a_led_pin_config_t *pin_config, Tca6408aPowerManager &power_manager)
{
    // 保存引脚配置
    pin_config_ = *pin_config;

    // 初始化红灯（GPIO_P0 已用于 LCD 背光控制，不能用于 LED）
    if (pin_config->red_gpio != TCA6408A_GPIO_P0 && pin_config->red_gpio <= TCA6408A_GPIO_P7)
    {
        red_led_ = new Tca6408aRedLed(tca_handle, pin_config->red_gpio);
        ESP_LOGI(TAG, "Red LED initialized on TCA6408A P%d", static_cast<int>(pin_config->red_gpio));
    }

    // 初始化绿灯（GPIO_P0 已用于 LCD 背光控制，不能用于 LED）
    if (pin_config->green_gpio != TCA6408A_GPIO_P0 && pin_config->green_gpio <= TCA6408A_GPIO_P7)
    {
        green_led_ = new Tca6408aGreenLed(tca_handle, pin_config->green_gpio);
        ESP_LOGI(TAG, "Green LED initialized on TCA6408A P%d", static_cast<int>(pin_config->green_gpio));
    }

    // 初始化后立即更新 LED 状态
    UpdateLedStatus(power_manager);

    ESP_LOGI(TAG, "TCA6408A LEDs initialized");
}

/**
 * @brief 统一更新所有 LED 状态
 *
 * @param power_manager 电源管理器引用
 */
void Tca6408aLedController::UpdateLedStatus(Tca6408aPowerManager &power_manager)
{
    auto device_power_state = power_manager.GetDevicePowerState();
    auto power_state = power_manager.GetPowerState();

    switch (device_power_state)
    {
    case DevicePowerState::CHARGING:
        // 充电状态：绿灯熄灭，红灯根据电源状态闪烁/常亮
        if (red_led_)
        {
            red_led_->UpdateBatteryStatus(power_state);
        }
        if (green_led_)
        {
            green_led_->TurnOff();
            green_led_->SetIgnoreStateChanges(true); // 设置绿灯忽略状态变化
        }
        break;

    case DevicePowerState::POWER_ON:
        // 开机状态：两个灯都工作
        if (red_led_)
        {
            red_led_->UpdateBatteryStatus(power_state);
        }
        if (green_led_)
        {
            green_led_->SetIgnoreStateChanges(false); // 恢复绿灯响应状态变化
            green_led_->OnStateChanged();
        }
        break;

    case DevicePowerState::POWER_OFF:
        // 关机状态：两个灯都熄灭
        if (red_led_)
        {
            red_led_->TurnOff();
        }
        if (green_led_)
        {
            green_led_->TurnOff();
            green_led_->SetIgnoreStateChanges(true); // 设置绿灯忽略状态变化
        }
        break;

    default:
        break;
    }
}

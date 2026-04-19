#include "tca6408a_power_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include "application.h"
#include "assets/lang_config.h"

const char *Tca6408aPowerManager::TAG = "Tca6408aPowerMgr";

Tca6408aPowerManager::Tca6408aPowerManager()
    : tca6408a_handle_(nullptr),
      power_state_(PowerState::NO_POWER),
      device_power_state_(DevicePowerState::CHARGING),
      low_battery_warning_(false),
      low_battery_shutdown_(false),
      battery_level_(0),
      battery_check_timer_(nullptr),
      battery_monitor_(nullptr)
{
}

Tca6408aPowerManager::~Tca6408aPowerManager()
{
    if (battery_check_timer_)
    {
        esp_timer_stop(battery_check_timer_);
        esp_timer_delete(battery_check_timer_);
        battery_check_timer_ = nullptr;
    }

    if (battery_monitor_)
    {
        delete battery_monitor_;
        battery_monitor_ = nullptr;
    }
}

void Tca6408aPowerManager::Initialize(tca6408a_handle_t *tca6408a_handle, const power_pin_config_t *pin_config)
{
    if (!tca6408a_handle || !pin_config)
    {
        ESP_LOGE(TAG, "Invalid arguments");
        return;
    }

    tca6408a_handle_ = tca6408a_handle;
    pin_config_ = *pin_config;

    // 配置 TCA6408A P5 为输出模式（电源保持）
    tca6408a_set_gpio_direction(tca6408a_handle_, pin_config->hold_gpio, TCA6408A_DIR_OUTPUT);
    // tca6408a_set_gpio_level(tca6408a_handle_, pin_config->hold_gpio, 0);

    // 配置 TCA6408A P6 为输入模式（充电完成检测）
    tca6408a_set_gpio_direction(tca6408a_handle_, pin_config->charge_done_gpio, TCA6408A_DIR_INPUT);

    // 配置 TCA6408A P7 为输入模式（充电中检测）
    tca6408a_set_gpio_direction(tca6408a_handle_, pin_config->charging_gpio, TCA6408A_DIR_INPUT);

    ESP_LOGI(TAG, "Configured TCA6408A pins: P%d(hold), P6(charge_done), P7(charging)", pin_config->hold_gpio);

    // 初始化 ADC 电池检测（使用原生 GPIO 引脚，充电完成引脚用 NC 代替）
    adc_channel_t adc_channel;
    if (pin_config->adc_gpio >= 1 && pin_config->adc_gpio <= 10)
    {
        adc_channel = (adc_channel_t)(pin_config->adc_gpio - 1);
        ESP_LOGI(TAG, "Configured ADC pin: GPIO%d, Channel: ADC_CHANNEL_%d", pin_config->adc_gpio, adc_channel);
    }
    else
    {
        ESP_LOGW(TAG, "Invalid ADC pin: GPIO%d. Using default ADC_CHANNEL_9", pin_config->adc_gpio);
        adc_channel = ADC_CHANNEL_9;
    }

    // 注意：charge_done_gpio 参数传入 GPIO_NUM_NC，因为 TCA6408A 的 P6 无法通过 ADC 初始化
    battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, adc_channel, 2.0f, 1.0f, GPIO_NUM_NC);

    // 更新初始电源状态
    UpdatePowerState();

    // 创建电源状态检查定时器
    esp_timer_create_args_t timer_args = {
        .callback = &Tca6408aPowerManager::PowerStateUpdateTimerCallback,
        .arg = this,
        .name = "tca6408a_power_state_timer"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &battery_check_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(battery_check_timer_, 5 * 1000 * 1000));

    ESP_LOGI(TAG, "TCA6408A Power Manager initialized");
}

void Tca6408aPowerManager::PowerOn()
{
    SetPowerState(true);
    tca6408a_set_gpio_level(tca6408a_handle_, pin_config_.hold_gpio, 1);
    SetDevicePowerState(DevicePowerState::POWER_ON);
    ESP_LOGI(TAG, "Power ON");
}

void Tca6408aPowerManager::PowerOff()
{
    SetPowerState(false);
    tca6408a_set_gpio_level(tca6408a_handle_, pin_config_.hold_gpio, 0);

    if (IsUsbPowered())
    {
        SetDevicePowerState(DevicePowerState::CHARGING);
    }
    else
    {
        SetDevicePowerState(DevicePowerState::POWER_OFF);
    }
    ESP_LOGI(TAG, "Power OFF");
}

uint8_t Tca6408aPowerManager::ReadBatteryLevel()
{
    return battery_monitor_->GetBatteryLevel();
}

void Tca6408aPowerManager::UpdatePowerState()
{
    battery_level_ = ReadBatteryLevel();

    // 读取 TCA6408A P6 和 P7 的状态
    uint8_t p6_level, p7_level;
    tca6408a_get_gpio_level(tca6408a_handle_, pin_config_.charge_done_gpio, &p6_level);
    tca6408a_get_gpio_level(tca6408a_handle_, pin_config_.charging_gpio, &p7_level);

    bool is_charging = (p7_level == 0);    // P7 低电平表示正在充电
    bool is_charge_done = (p6_level == 0); // P6 低电平表示充电完成
    bool battery_detected = battery_level_ > 20;

    PowerState previous_state = power_state_;

    if (is_charging && battery_detected)
    {
        power_state_ = PowerState::USB_POWER_CHARGING;
    }
    else if (is_charge_done && battery_detected)
    {
        power_state_ = PowerState::USB_POWER_DONE;
    }
    else if (is_charging && !battery_detected)
    {
        power_state_ = PowerState::USB_POWER_NO_BATTERY;
    }
    else if (is_charge_done && !battery_detected)
    {
        power_state_ = PowerState::USB_POWER_NO_BATTERY;
    }
    else if (battery_detected && !low_battery_warning_)
    {
        power_state_ = PowerState::BATTERY_POWER;
    }
    else if (battery_detected && low_battery_warning_)
    {
        power_state_ = PowerState::LOW_BATTERY;
    }
    else
    {
        power_state_ = PowerState::NO_POWER;
    }

    if (previous_state != power_state_ && power_state_callback_)
    {
        power_state_callback_(power_state_);
    }

    ESP_LOGD(TAG, "Battery: %d%%, P6=%d, P7=%d, State: %d",
             battery_level_, p6_level, p7_level, static_cast<int>(power_state_));
}

void Tca6408aPowerManager::CheckLowBattery()
{
    battery_level_ = ReadBatteryLevel();

    if (power_state_ == PowerState::BATTERY_POWER || power_state_ == PowerState::LOW_BATTERY)
    {
        if (battery_level_ < 40 && !low_battery_shutdown_)
        {
            ESP_LOGW(TAG, "Critical battery (%d%%), shutting down", battery_level_);
            low_battery_shutdown_ = true;

            auto &app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(500));

            PowerOff();
            ESP_LOGI(TAG, "Device shut down due to critical battery");
            return;
        }
        else if (battery_level_ < 50 && battery_level_ >= 40 && !low_battery_warning_)
        {
            ESP_LOGW(TAG, "Low battery warning (%d%%)", battery_level_);
            low_battery_warning_ = true;

            auto &app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else if (battery_level_ >= 20)
        {
            low_battery_warning_ = false;
        }
    }
    else if (power_state_ == PowerState::USB_POWER_NO_BATTERY)
    {
        low_battery_warning_ = false;
        low_battery_shutdown_ = false;
        ESP_LOGI(TAG, "USB powered, no battery detected");
    }
    else
    {
        low_battery_warning_ = false;
        low_battery_shutdown_ = false;
    }
}

void Tca6408aPowerManager::PowerStateUpdateTimerCallback(void *arg)
{
    Tca6408aPowerManager *self = static_cast<Tca6408aPowerManager *>(arg);
    self->CheckLowBattery();
    self->UpdatePowerState();
}

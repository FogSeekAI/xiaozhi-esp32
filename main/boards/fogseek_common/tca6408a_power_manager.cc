#include "tca6408a_power_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include "application.h"
#include "assets/lang_config.h"

const char *TCA6408APowerManager::TAG = "TCA6408APowerManager";

/**
 * @brief 构造函数
 */
TCA6408APowerManager::TCA6408APowerManager() 
    : power_state_(PowerState::NO_POWER),
      device_power_state_(DevicePowerState::CHARGING),
      low_battery_warning_(false),
      low_battery_shutdown_(false),
      battery_level_(0),
      battery_check_timer_(nullptr),
      battery_monitor_(nullptr),
      power_state_callback_(nullptr)
{
    memset(&io_expander_, 0, sizeof(io_expander_));
}

/**
 * @brief 析构函数
 */
TCA6408APowerManager::~TCA6408APowerManager()
{
    // 清理定时器
    if (battery_check_timer_)
    {
        esp_timer_stop(battery_check_timer_);
        esp_timer_delete(battery_check_timer_);
        battery_check_timer_ = nullptr;
    }

    // 清理电池监控器
    if (battery_monitor_)
    {
        delete battery_monitor_;
        battery_monitor_ = nullptr;
    }

    // 清理 IO 扩展器
    if (io_expander_.initialized)
    {
        tca6408a_deinit(&io_expander_);
    }
}

/**
 * @brief 初始化电源管理器
 */
esp_err_t TCA6408APowerManager::Initialize(const power_pin_config_t *pin_config, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address)
{
    if (!pin_config || !i2c_bus)
    {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    pin_config_ = *pin_config;

    // 初始化 TCA6408A
    tca6408a_config_t io_config = {
        .i2c_bus = i2c_bus,
        .i2c_address = i2c_address,
        .int_gpio = GPIO_NUM_NC,
        .reset_gpio = GPIO_NUM_NC
    };

    esp_err_t ret = tca6408a_init(&io_expander_, &io_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize TCA6408A: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "TCA6408A initialized at 0x%02X", i2c_address);

    // 初始化输出引脚：关闭所有输出
    // P0: LCD 背光，P1: 功放，P2: 红灯，P3: 绿灯，P5: PWR_HOLD
    ret = tca6408a_set_all_gpio_level(&io_expander_, 0x00);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set initial GPIO levels");
        return ret;
    }

    // 配置 ADC 引脚
    adc_channel_t adc_channel;
    if (pin_config_->adc_gpio >= 1 && pin_config_->adc_gpio <= 10)
    {
        adc_channel = (adc_channel_t)(pin_config_->adc_gpio - 1);
        ESP_LOGI(TAG, "Configured ADC pin: GPIO%d, Channel: ADC_CHANNEL_%d", 
                 pin_config_->adc_gpio, adc_channel);
    }
    else
    {
        ESP_LOGW(TAG, "Invalid ADC pin: GPIO%d. Using ADC_CHANNEL_9", pin_config_->adc_gpio);
        adc_channel = ADC_CHANNEL_9;
    }

    battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, adc_channel, 2.0f, 1.0f, 
                                             (gpio_num_t)pin_config_->charge_done_gpio);

    // 更新初始电源状态
    UpdatePowerState();

    // 创建电源状态检查定时器
    esp_timer_create_args_t timer_args = {
        .callback = &TCA6408APowerManager::PowerStateUpdateTimerCallback,
        .arg = this,
        .name = "power_state_update_timer"};
    
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &battery_check_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(battery_check_timer_, 5 * 1000 * 1000)); // 5 秒

    ESP_LOGI(TAG, "TCA6408A Power Manager initialized successfully");
    return ESP_OK;
}

/**
 * @brief 开机
 */
void TCA6408APowerManager::PowerOn()
{
    SetPowerState(true);
    
    // 通过 IO 扩展器设置 PWR_HOLD (P5) 为高电平
    esp_err_t ret = tca6408a_set_gpio_level(&io_expander_, TCA6408A_GPIO_P5, 1);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set PWR_HOLD high");
        return;
    }
    
    SetDevicePowerState(DevicePowerState::POWER_ON);
    ESP_LOGI(TAG, "Power ON");
}

/**
 * @brief 关机
 */
void TCA6408APowerManager::PowerOff()
{
    SetPowerState(false);
    
    // 通过 IO 扩展器设置 PWR_HOLD (P5) 为低电平
    esp_err_t ret = tca6408a_set_gpio_level(&io_expander_, TCA6408A_GPIO_P5, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set PWR_HOLD low");
    }
    
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

/**
 * @brief 读取电池电量
 */
uint8_t TCA6408APowerManager::ReadBatteryLevel()
{
    return battery_monitor_->GetBatteryLevel();
}

/**
 * @brief 更新电源状态
 */
void TCA6408APowerManager::UpdatePowerState()
{
    battery_level_ = ReadBatteryLevel();

    // 通过 IO 扩展器读取充电状态引脚
    uint8_t charging_level, charge_done_level;
    esp_err_t ret_charging = tca6408a_get_gpio_level(&io_expander_, TCA6408A_GPIO_P7, &charging_level);
    esp_err_t ret_done = tca6408a_get_gpio_level(&io_expander_, TCA6408A_GPIO_P6, &charge_done_level);
    
    if (ret_charging != ESP_OK || ret_done != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read charging status");
        return;
    }

    bool is_charging = (charging_level == 0);
    bool is_charge_done = (charge_done_level == 0);
    bool battery_detected = battery_level_ > 20;

    PowerState previous_state = power_state_;

    // 判断电源状态
    if (is_charging && battery_detected)
    {
        power_state_ = PowerState::USB_POWER_CHARGING;
    }
    else if (is_charge_done && battery_detected)
    {
        power_state_ = PowerState::USB_POWER_DONE;
    }
    else if ((is_charging || is_charge_done) && !battery_detected)
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

    // 状态变化时调用回调
    if (previous_state != power_state_ && power_state_callback_)
    {
        power_state_callback_(power_state_);
    }

    ESP_LOGD(TAG, "Battery level: %d%%, Power state: %d", 
             battery_level_, static_cast<int>(power_state_));
}

/**
 * @brief 检查低电量
 */
void TCA6408APowerManager::CheckLowBattery()
{
    battery_level_ = ReadBatteryLevel();

    if (power_state_ == PowerState::BATTERY_POWER || power_state_ == PowerState::LOW_BATTERY)
    {
        // 低于 40% 自动关机
        if (battery_level_ < 40 && !low_battery_shutdown_)
        {
            ESP_LOGW(TAG, "Critical battery level (%d%%), shutting down", battery_level_);
            low_battery_shutdown_ = true;

            auto &app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(500));

            PowerOff();
            ESP_LOGI(TAG, "Device shut down due to critical battery");
            return;
        }
        // 低于 50% 警告
        else if (battery_level_ < 50 && battery_level_ >= 40 && !low_battery_warning_)
        {
            ESP_LOGW(TAG, "Low battery warning (%d%%)", battery_level_);
            low_battery_warning_ = true;

            auto &app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        // 电量恢复
        else if (battery_level_ >= 20)
        {
            low_battery_warning_ = false;
        }
    }
    else if (power_state_ == PowerState::USB_POWER_NO_BATTERY)
    {
        low_battery_warning_ = false;
        low_battery_shutdown_ = false;
        ESP_LOGI(TAG, "USB powered with no battery");
    }
    else
    {
        low_battery_warning_ = false;
        low_battery_shutdown_ = false;
    }
}

/**
 * @brief 电源状态更新定时器回调
 */
void TCA6408APowerManager::PowerStateUpdateTimerCallback(void *arg)
{
    TCA6408APowerManager *self = static_cast<TCA6408APowerManager *>(arg);
    self->CheckLowBattery();
    self->UpdatePowerState();
}

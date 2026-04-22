#ifndef _TCA6408A_POWER_MANAGER_H_
#define _TCA6408A_POWER_MANAGER_H_

#include "tca6408a_io_expander.h"
#include "adc_battery_monitor.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include <functional>

enum class PowerState
{
    NO_POWER = 0,
    USB_POWER_CHARGING,
    USB_POWER_DONE,
    USB_POWER_NO_BATTERY,
    BATTERY_POWER,
    LOW_BATTERY
};

enum class DevicePowerState
{
    POWER_OFF = 0,
    POWER_ON,
    CHARGING
};

class Tca6408aPowerManager
{
public:
    struct power_pin_config_t
    {
        tca6408a_gpio_t hold_gpio;        // TCA6408A P5 - 电源保持
        tca6408a_gpio_t charging_gpio;    // TCA6408A P7 - 充电中检测
        tca6408a_gpio_t charge_done_gpio; // TCA6408A P6 - 充电完成检测
        gpio_num_t adc_gpio;              // 原生 GPIO - ADC 电池检测引脚
    };

    using PowerStateCallback = std::function<void(PowerState)>;

    Tca6408aPowerManager();
    ~Tca6408aPowerManager();

    void Initialize(tca6408a_handle_t *tca6408a_handle, const power_pin_config_t *pin_config);
    void PowerOn();
    void PowerOff();

    // 设备电源开关机状态
    void SetDevicePowerState(DevicePowerState state) { device_power_state_ = state; }
    void SetPowerState(bool is_power_on) { is_power_on_ = is_power_on; }
    bool IsPowerOn() const { return is_power_on_; }
    DevicePowerState GetDevicePowerState() const { return device_power_state_; }

    // 电源供电状态
    bool IsBatteryPowered() const { return power_state_ == PowerState::BATTERY_POWER ||
                                           power_state_ == PowerState::LOW_BATTERY; }
    bool IsUsbPowered() const
    {
        return power_state_ == PowerState::USB_POWER_CHARGING ||
               power_state_ == PowerState::USB_POWER_DONE ||
               power_state_ == PowerState::USB_POWER_NO_BATTERY;
    }
    PowerState GetPowerState() const { return power_state_; }

    // 读取电池电量
    uint8_t ReadBatteryLevel();

    // 回调设置
    void SetPowerStateCallback(PowerStateCallback callback) { power_state_callback_ = callback; }

private:
    void UpdatePowerState();
    void CheckLowBattery();
    static void PowerStateUpdateTimerCallback(void *arg);

    tca6408a_handle_t *tca6408a_handle_;
    power_pin_config_t pin_config_;

    PowerState power_state_;
    DevicePowerState device_power_state_;

    bool is_power_on_;
    bool low_battery_warning_;
    bool low_battery_shutdown_;
    uint8_t battery_level_;

    esp_timer_handle_t battery_check_timer_;
    AdcBatteryMonitor *battery_monitor_;

    PowerStateCallback power_state_callback_;

    static const char *TAG;
};

#endif // _TCA6408A_POWER_MANAGER_H_

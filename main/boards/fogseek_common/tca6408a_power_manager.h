#ifndef _TCA6408A_POWER_MANAGER_H_
#define _TCA6408A_POWER_MANAGER_H_

#include "tca6408a_io_expander.h"
#include "boards/common/adc_battery_monitor.h"
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <functional>

/**
 * @brief 基于 TCA6408A IO 扩展器的电源管理器
 * 
 * 功能特性:
 * - 通过 TCA6408A 控制电源保持引脚 (P5)
 * - 通过 TCA6408A 读取充电状态 (P6, P7)
 * - 电池电量检测
 * - 低电量保护
 * - 电源状态回调
 */
class TCA6408APowerManager
{
public:
    // 电源状态枚举
    enum class PowerState
    {
        USB_POWER_CHARGING,   // USB 供电充电中
        USB_POWER_DONE,       // USB 供电充电完成
        USB_POWER_NO_BATTERY, // USB 供电无电池
        BATTERY_POWER,        // 电池供电
        LOW_BATTERY,          // 低电量状态
        NO_POWER              // 无电源
    };

    // 设备开关机状态枚举
    enum class DevicePowerState
    {
        CHARGING, // 充电状态
        POWER_ON, // 开机状态
        POWER_OFF // 关机状态
    };

    // 电源状态变化回调函数类型
    using PowerStateCallback = std::function<void(PowerState)>;

    /**
     * @brief 电源引脚配置结构体
     */
    typedef struct
    {
        int charging_gpio;      // 充电中检测 GPIO（原生）
        int charge_done_gpio;   // 充电完成检测 GPIO（原生）
        int adc_gpio;           // ADC 引脚
    } power_pin_config_t;

    TCA6408APowerManager();
    ~TCA6408APowerManager();

    /**
     * @brief 初始化电源管理器
     * 
     * @param pin_config 引脚配置
     * @param i2c_bus I2C 总线句柄
     * @param i2c_address TCA6408A I2C 地址（默认 0x20）
     * @return esp_err_t 错误代码
     */
    esp_err_t Initialize(const power_pin_config_t *pin_config, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address = 0x20);

    /**
     * @brief 开机
     */
    void PowerOn();

    /**
     * @brief 关机
     */
    void PowerOff();

    /**
     * @brief 设置设备电源状态
     */
    void SetDevicePowerState(DevicePowerState state) { device_power_state_ = state; }

    /**
     * @brief 获取设备电源状态
     */
    DevicePowerState GetDevicePowerState() const { return device_power_state_; }

    /**
     * @brief 设置电源开关标志
     */
    void SetPowerState(bool is_power_on) { is_power_on_ = is_power_on_; }

    /**
     * @brief 获取电源开关标志
     */
    bool IsPowerOn() const { return is_power_on_; }

    /**
     * @brief 是否电池供电
     */
    bool IsBatteryPowered() const { return power_state_ == PowerState::BATTERY_POWER ||
                                           power_state_ == PowerState::LOW_BATTERY; }

    /**
     * @brief 是否 USB 供电
     */
    bool IsUsbPowered() const
    {
        return power_state_ == PowerState::USB_POWER_CHARGING ||
               power_state_ == PowerState::USB_POWER_DONE ||
               power_state_ == PowerState::USB_POWER_NO_BATTERY;
    }

    /**
     * @brief 获取电源状态
     */
    PowerState GetPowerState() const { return power_state_; }

    /**
     * @brief 读取电池电量
     */
    uint8_t ReadBatteryLevel();

    /**
     * @brief 设置电源状态变化回调
     */
    void SetPowerStateCallback(PowerStateCallback callback) { power_state_callback_ = callback; }

private:
    static const char *TAG;

    // 成员变量
    tca6408a_handle_t io_expander_;                 // IO 扩展器句柄
    power_pin_config_t pin_config_;                 // 引脚配置
    PowerState power_state_ = PowerState::NO_POWER; // 电源状态
    DevicePowerState device_power_state_ = DevicePowerState::CHARGING; // 设备电源状态
    bool is_power_on_ = false;                      // 电源是否开启
    bool low_battery_warning_ = false;              // 低电量警告标志
    bool low_battery_shutdown_ = false;             // 低电量关机标志
    uint8_t battery_level_ = 0;                     // 电池电量百分比
    esp_timer_handle_t battery_check_timer_ = nullptr; // 电池检查定时器
    AdcBatteryMonitor *battery_monitor_ = nullptr;  // 电池监控器
    PowerStateCallback power_state_callback_ = nullptr; // 电源状态回调

    // 私有方法
    void UpdatePowerState();
    void CheckLowBattery();
    static void PowerStateUpdateTimerCallback(void *arg);
};

#endif

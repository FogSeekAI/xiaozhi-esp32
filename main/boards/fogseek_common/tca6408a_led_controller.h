#ifndef _TCA6408A_LED_CONTROLLER_H_
#define _TCA6408A_LED_CONTROLLER_H_

#include "tca6408a_led.h"
#include "tca6408a_io_expander.h"
#include "tca6408a_power_manager.h"
#include "../../application.h"
#include <esp_log.h>
#include <memory>

// LED 引脚配置结构体（针对 TCA6408A）
typedef struct
{
    tca6408a_gpio_t red_gpio;   // 红色 LED GPIO 引脚
    tca6408a_gpio_t green_gpio; // 绿色 LED GPIO 引脚
} tca6408a_led_pin_config_t;

/**
 * @brief 基于 TCA6408A 的红色 LED 类，负责电源状态显示
 */
class Tca6408aRedLed : public Tca6408aLed
{
public:
    Tca6408aRedLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio);
    ~Tca6408aRedLed();

    // 空实现，因为红灯不响应设备状态变化
    void OnStateChanged() override;

    // 响应电源状态变化
    void UpdateBatteryStatus(PowerState state);

private:
    static const char *TAG;
};

/**
 * @brief 基于 TCA6408A 的绿色 LED 类，负责设备状态显示
 */
class Tca6408aGreenLed : public Tca6408aLed
{
public:
    Tca6408aGreenLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio);
    ~Tca6408aGreenLed();

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
 * @brief 基于 TCA6408A 的 LED 控制器类
 *
 * 该类管理通过 TCA6408A IO 扩展器控制的两个 LED（红灯和绿灯）。
 * 由于 TCA6408A 不支持 PWM，因此只提供基础的开关和闪烁功能。
 */
class Tca6408aLedController
{
public:
    Tca6408aLedController();
    ~Tca6408aLedController();

    /**
     * @brief 初始化 LED
     * 
     * @param tca_handle TCA6408A 驱动句柄指针
     * @param pin_config LED 引脚配置
     * @param power_manager 电源管理器引用
     */
    void InitializeLeds(tca6408a_handle_t *tca_handle, const tca6408a_led_pin_config_t *pin_config, Tca6408aPowerManager &power_manager);

    /**
     * @brief 统一更新所有 LED 状态
     * 
     * @param power_manager 电源管理器引用
     */
    void UpdateLedStatus(Tca6408aPowerManager &power_manager);

    /**
     * @brief 获取红色 LED 实例
     * @return 红色 LED 指针
     */
    Tca6408aRedLed *GetRedLed() const { return red_led_; }

    /**
     * @brief 获取绿色 LED 实例
     * @return 绿色 LED 指针
     */
    Tca6408aGreenLed *GetGreenLed() const { return green_led_; }

private:
    static const char *TAG; // 日志标签

    // LED 实例
    Tca6408aRedLed *red_led_ = nullptr;     // 红色 LED 控制器实例
    Tca6408aGreenLed *green_led_ = nullptr; // 绿色 LED 控制器实例

    tca6408a_led_pin_config_t pin_config_; // LED 引脚配置
};

#endif // _TCA6408A_LED_CONTROLLER_H_

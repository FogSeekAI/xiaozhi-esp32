#ifndef _TCA6408A_LED_H_
#define _TCA6408A_LED_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../../led/led.h"
#include "tca6408a_io_expander.h"
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class Tca6408aLed : public Led
{
public:
    /**
     * @brief 构造函数
     * 
     * @param tca_handle TCA6408A 驱动句柄指针
     * @param gpio TCA6408A GPIO 引脚编号（P0-P7）
     */
    Tca6408aLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio);
    virtual ~Tca6408aLed();

    void OnStateChanged() override;
    
    /**
     * @brief 点亮 LED
     */
    void TurnOn();
    
    /**
     * @brief 熄灭 LED
     */
    void TurnOff();
    
    /**
     * @brief 闪烁指定次数
     * 
     * @param times 闪烁次数
     * @param interval_ms 每次闪烁的间隔时间（毫秒）
     */
    void Blink(int times, int interval_ms);
    
    /**
     * @brief 单次闪烁
     */
    void BlinkOnce();
    
    /**
     * @brief 开始连续闪烁
     * 
     * @param interval_ms 闪烁间隔时间（毫秒）
     */
    void StartContinuousBlink(int interval_ms);
    
    /**
     * @brief 停止闪烁
     */
    void StopBlink();

private:
    std::mutex mutex_;
    tca6408a_handle_t *tca_handle_;  // TCA6408A 驱动句柄
    tca6408a_gpio_t gpio_;           // GPIO 引脚编号
    esp_timer_handle_t blink_timer_; // 闪烁定时器
    bool is_blinking_;               // 是否正在闪烁
    int blink_counter_;              // 闪烁计数器（剩余次数）
    int blink_interval_ms_;          // 闪烁间隔时间
    bool led_state_;                 // 当前 LED 状态

    /**
     * @brief 内部停止闪烁方法（不获取mutex，由调用者保证）
     */
    void StopBlinkInternal();

    /**
     * @brief 定时器回调函数
     */
    static void OnBlinkTimer(void *arg);
    
    /**
     * @brief 处理定时器事件
     */
    void HandleBlinkTimer();
};

#endif // _TCA6408A_LED_H_

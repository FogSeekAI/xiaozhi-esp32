#ifndef _TCA6408A_BUTTON_H_
#define _TCA6408A_BUTTON_H_

#include "tca6408a_io_expander.h"
#include "tca6408a_interrupt_manager.h"
#include <driver/gpio.h>
#include <functional>
#include <esp_timer.h>

class Tca6408aButton
{
public:
    Tca6408aButton();
    ~Tca6408aButton();

    void Initialize(tca6408a_handle_t *tca6408a_handle, tca6408a_gpio_t gpio, bool active_low = true);
    void Initialize(Tca6408aInterruptManager *interrupt_manager);

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);

private:
    void HandleStateChange(uint8_t level);
    static void TimerCallback(void *arg);

    tca6408a_handle_t *tca6408a_handle_ = nullptr;
    tca6408a_gpio_t gpio_ = TCA6408A_GPIO_P0;
    bool active_low_ = true;
    bool is_pressed_ = false;
    uint8_t click_count_ = 0;

    esp_timer_handle_t timer_ = nullptr;
    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_long_press_;
};

#endif // _TCA6408A_BUTTON_H_

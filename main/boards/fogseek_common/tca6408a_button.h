#ifndef _TCA6408A_BUTTON_H_
#define _TCA6408A_BUTTON_H_

#include "tca6408a_io_expander.h"
#include "tca6408a_interrupt_manager.h"
#include <driver/gpio.h>
#include <functional>
#include <esp_timer.h>

class TCA6408AButton
{
public:
    TCA6408AButton(tca6408a_handle_t *tca6408a_handle, tca6408a_gpio_t gpio, bool active_low = true);
    ~TCA6408AButton();

    void Initialize(TCA6408AInterruptManager *interrupt_manager);

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);

private:
    void HandleStateChange(uint8_t level);
    static void TimerCallback(void *arg);

    tca6408a_handle_t *tca6408a_handle_;
    tca6408a_gpio_t gpio_;
    bool active_low_;
    bool is_pressed_;
    uint8_t click_count_;

    esp_timer_handle_t timer_;
    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_long_press_;
};

#endif // _TCA6408A_BUTTON_H_
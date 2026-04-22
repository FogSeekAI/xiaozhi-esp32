#ifndef _TCA6408A_INTERRUPT_MANAGER_H_
#define _TCA6408A_INTERRUPT_MANAGER_H_

#include "tca6408a_io_expander.h"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <map>

class Tca6408aInterruptManager
{
public:
    using InputPinCallback = std::function<void(uint8_t gpio, uint8_t level)>;

    struct InputPinConfig
    {
        tca6408a_gpio_t gpio;
        InputPinCallback callback;
        uint8_t last_level;
        bool enabled;
    };

    Tca6408aInterruptManager();
    ~Tca6408aInterruptManager();

    void Initialize(tca6408a_handle_t *tca6408a_handle, gpio_num_t int_gpio);

    void RegisterInputPin(tca6408a_gpio_t gpio, InputPinCallback callback);
    void UnregisterInputPin(tca6408a_gpio_t gpio);
    void EnableInputPin(tca6408a_gpio_t gpio);
    void DisableInputPin(tca6408a_gpio_t gpio);

private:
    static void IRAM_ATTR InterruptHandler(void *arg);
    static void InterruptTask(void *arg);
    void HandleInputChange(uint8_t input_data);
    uint8_t ReadInputState();

    tca6408a_handle_t *tca6408a_handle_ = nullptr;
    gpio_num_t int_gpio_ = GPIO_NUM_NC;
    TaskHandle_t task_handle_ = nullptr;
    std::map<tca6408a_gpio_t, InputPinConfig> input_pins_;
    uint8_t last_input_state_ = 0xFF;
};

#endif // _TCA6408A_INTERRUPT_MANAGER_H_

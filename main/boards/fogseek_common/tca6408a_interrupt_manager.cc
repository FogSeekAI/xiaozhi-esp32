#include "tca6408a_interrupt_manager.h"
#include <esp_log.h>

#define TAG "Tca6408aIntMgr"

Tca6408aInterruptManager::Tca6408aInterruptManager()
    : tca6408a_handle_(nullptr),
      int_gpio_(GPIO_NUM_NC),
      task_handle_(nullptr),
      last_input_state_(0xFF)
{
}

Tca6408aInterruptManager::~Tca6408aInterruptManager()
{
    if (task_handle_)
    {
        vTaskDelete(task_handle_);
    }

    if (int_gpio_ != GPIO_NUM_NC && int_gpio_ >= 0)
    {
        gpio_isr_handler_remove(int_gpio_);
    }
}

void Tca6408aInterruptManager::Initialize(tca6408a_handle_t *tca6408a_handle, gpio_num_t int_gpio)
{
    tca6408a_handle_ = tca6408a_handle;
    int_gpio_ = int_gpio;

    if (!tca6408a_handle_ || int_gpio_ == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Init failed: invalid handle or GPIO");
        return;
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << int_gpio_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    static bool isr_installed = false;
    if (!isr_installed) {
        gpio_install_isr_service(ESP_INTR_FLAG_SHARED);
        isr_installed = true;
    }
    gpio_isr_handler_add(int_gpio_, InterruptHandler, this);

    xTaskCreate(InterruptTask, "tca6408a_int_task", 2048, this, 10, &task_handle_);
    ESP_LOGI(TAG, "Initialized on GPIO%d", int_gpio_);
}

void Tca6408aInterruptManager::RegisterInputPin(tca6408a_gpio_t gpio, InputPinCallback callback)
{
    if (gpio > TCA6408A_GPIO_P7)
    {
        ESP_LOGE(TAG, "Invalid GPIO: %d", gpio);
        return;
    }

    uint8_t current_level;
    esp_err_t ret = tca6408a_get_gpio_level(tca6408a_handle_, gpio, &current_level);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to read initial level for P%d, defaulting to 1", gpio);
        current_level = 1;
    }

    InputPinConfig config = {
        .gpio = gpio,
        .callback = callback,
        .last_level = current_level,
        .enabled = true};

    input_pins_[gpio] = config;

    ESP_LOGI(TAG, "Registered input pin P%d, initial level: %d", gpio, current_level);
}

void Tca6408aInterruptManager::UnregisterInputPin(tca6408a_gpio_t gpio)
{
    auto it = input_pins_.find(gpio);
    if (it != input_pins_.end())
    {
        input_pins_.erase(it);
        ESP_LOGI(TAG, "Unregistered input pin P%d", gpio);
    }
}

void Tca6408aInterruptManager::EnableInputPin(tca6408a_gpio_t gpio)
{
    auto it = input_pins_.find(gpio);
    if (it != input_pins_.end())
    {
        it->second.enabled = true;
        ESP_LOGD(TAG, "Enabled input pin P%d", gpio);
    }
}

void Tca6408aInterruptManager::DisableInputPin(tca6408a_gpio_t gpio)
{
    auto it = input_pins_.find(gpio);
    if (it != input_pins_.end())
    {
        it->second.enabled = false;
        ESP_LOGD(TAG, "Disabled input pin P%d", gpio);
    }
}

void IRAM_ATTR Tca6408aInterruptManager::InterruptHandler(void *arg)
{
    auto instance = static_cast<Tca6408aInterruptManager *>(arg);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(instance->task_handle_, 1, eSetValueWithoutOverwrite, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

void Tca6408aInterruptManager::InterruptTask(void *arg)
{
    auto instance = static_cast<Tca6408aInterruptManager *>(arg);

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t current_input_state = instance->ReadInputState();

        if (current_input_state != 0xFF && current_input_state != instance->last_input_state_)
        {
            instance->HandleInputChange(current_input_state);
            instance->last_input_state_ = current_input_state;
        }
    }
}

uint8_t Tca6408aInterruptManager::ReadInputState()
{
    uint8_t level;
    esp_err_t ret = tca6408a_get_all_gpio_level(tca6408a_handle_, &level);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read input state: %d", ret);
        return 0xFF;
    }
    return level;
}

void Tca6408aInterruptManager::HandleInputChange(uint8_t input_data)
{
    for (auto &pair : input_pins_)
    {
        InputPinConfig &config = pair.second;

        if (!config.enabled)
        {
            continue;
        }

        uint8_t current_level = (input_data >> config.gpio) & 0x01;

        if (current_level != config.last_level)
        {
            ESP_LOGD(TAG, "P%d state changed: %d -> %d", config.gpio, config.last_level, current_level);

            if (config.callback)
            {
                config.callback(config.gpio, current_level);
            }

            config.last_level = current_level;
        }
    }
}
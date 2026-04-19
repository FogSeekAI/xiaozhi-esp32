#include "tca6408a_button.h"
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "Tca6408aButton"
#define LONG_PRESS_TIME_MS 2000
#define DOUBLE_CLICK_TIME_MS 300

Tca6408aButton::Tca6408aButton()
    : tca6408a_handle_(nullptr),
      gpio_(TCA6408A_GPIO_P0),
      active_low_(true),
      is_pressed_(false),
      click_count_(0),
      timer_(nullptr)
{
}

Tca6408aButton::~Tca6408aButton()
{
    if (timer_)
    {
        esp_timer_delete(timer_);
    }
}

void Tca6408aButton::Initialize(tca6408a_handle_t *tca6408a_handle, tca6408a_gpio_t gpio, bool active_low)
{
    tca6408a_handle_ = tca6408a_handle;
    gpio_ = gpio;
    active_low_ = active_low;
}

void Tca6408aButton::Initialize(Tca6408aInterruptManager *interrupt_manager)
{
    if (!interrupt_manager || !tca6408a_handle_)
    {
        ESP_LOGE(TAG, "Invalid interrupt manager or handle");
        return;
    }

    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tca6408a_btn_timer"};

    esp_err_t ret = esp_timer_create(&timer_args, &timer_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create timer: %d", ret);
        return;
    }

    auto callback = [this](uint8_t gpio, uint8_t level)
    {
        if (gpio == gpio_)
        {
            HandleStateChange(level);
        }
    };

    interrupt_manager->RegisterInputPin(gpio_, callback);

    // 检查初始化时按键是否已经按下，处理开机时按键已按下的场景
    uint8_t initial_level;
    esp_err_t err = tca6408a_get_gpio_level(tca6408a_handle_, gpio_, &initial_level);
    if (err == ESP_OK)
    {
        bool initially_pressed = active_low_ ? (initial_level == 0) : (initial_level == 1);
        if (initially_pressed)
        {
            ESP_LOGI(TAG, "Button P%d is already pressed at initialization", gpio_);
            is_pressed_ = true;
            esp_timer_start_once(timer_, LONG_PRESS_TIME_MS * 1000);
            
            if (on_press_down_)
            {
                on_press_down_();
            }
        }
    }

    ESP_LOGI(TAG, "Initialized on P%d, active_%s", gpio_, active_low_ ? "low" : "high");
}

void Tca6408aButton::HandleStateChange(uint8_t level)
{
    bool pressed = active_low_ ? (level == 0) : (level == 1);

    if (pressed && !is_pressed_)
    {
        is_pressed_ = true;
        esp_timer_stop(timer_);

        if (on_press_down_)
        {
            on_press_down_();
        }

        esp_timer_start_once(timer_, LONG_PRESS_TIME_MS * 1000);
    }
    else if (!pressed && is_pressed_)
    {
        is_pressed_ = false;
        esp_timer_stop(timer_);

        if (on_press_up_)
        {
            on_press_up_();
        }

        click_count_++;
        ESP_LOGD(TAG, "Click count: %d", click_count_);

        if (click_count_ >= 2)
        {
            if (on_double_click_)
            {
                on_double_click_();
            }
            click_count_ = 0;
            ESP_LOGD(TAG, "Double click detected");
        }
        else
        {
            esp_timer_start_once(timer_, DOUBLE_CLICK_TIME_MS * 1000);
        }
    }
}

void Tca6408aButton::TimerCallback(void *arg)
{
    auto btn = static_cast<Tca6408aButton *>(arg);

    if (btn->is_pressed_)
    {
        ESP_LOGD(TAG, "Long press timeout");
        if (btn->on_long_press_)
        {
            btn->on_long_press_();
        }
        // 长按触发后重置点击计数，避免松开时触发单击
        btn->click_count_ = 0;
    }
    else if (btn->click_count_ == 1)
    {
        ESP_LOGD(TAG, "Single click confirmed");
        if (btn->on_click_)
        {
            btn->on_click_();
        }
        btn->click_count_ = 0;
    }
}

void Tca6408aButton::OnPressDown(std::function<void()> callback)
{
    on_press_down_ = callback;
}

void Tca6408aButton::OnPressUp(std::function<void()> callback)
{
    on_press_up_ = callback;
}

void Tca6408aButton::OnClick(std::function<void()> callback)
{
    on_click_ = callback;
}

void Tca6408aButton::OnDoubleClick(std::function<void()> callback)
{
    on_double_click_ = callback;
}

void Tca6408aButton::OnLongPress(std::function<void()> callback)
{
    on_long_press_ = callback;
}
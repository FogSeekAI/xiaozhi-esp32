#include "tca6408a_led.h"
#include <esp_log.h>

static const char *TAG = "Tca6408aLed";

// 全局I2C互斥锁，保护所有TCA6408A操作
static std::mutex tca6408a_i2c_mutex;

Tca6408aLed::Tca6408aLed(tca6408a_handle_t *tca_handle, tca6408a_gpio_t gpio)
    : tca_handle_(tca_handle),
      gpio_(gpio),
      blink_timer_(nullptr),
      is_blinking_(false),
      blink_counter_(0),
      blink_interval_ms_(0),
      led_state_(false)
{
    // 确保 GPIO 配置为输出模式
    {
        std::lock_guard<std::mutex> lock(tca6408a_i2c_mutex);
        esp_err_t ret = tca6408a_set_gpio_direction(tca_handle_, gpio_, TCA6408A_DIR_OUTPUT);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set GPIO direction: %s", esp_err_to_name(ret));
        }
    }

    // 初始状态为熄灭
    TurnOff();
}

Tca6408aLed::~Tca6408aLed()
{
    StopBlink();

    // 删除定时器
    if (blink_timer_ != nullptr)
    {
        esp_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
}

void Tca6408aLed::TurnOn()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果正在闪烁，先停止
    if (is_blinking_)
    {
        StopBlinkInternal();
    }

    std::lock_guard<std::mutex> i2c_lock(tca6408a_i2c_mutex);
    esp_err_t ret = tca6408a_set_gpio_level(tca_handle_, gpio_, 1);
    if (ret == ESP_OK)
    {
        led_state_ = true;
        ESP_LOGD(TAG, "LED turned on");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to turn on LED: %s", esp_err_to_name(ret));
    }
}

void Tca6408aLed::TurnOff()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果正在闪烁，先停止
    if (is_blinking_)
    {
        StopBlinkInternal();
    }

    std::lock_guard<std::mutex> i2c_lock(tca6408a_i2c_mutex);
    esp_err_t ret = tca6408a_set_gpio_level(tca_handle_, gpio_, 0);
    if (ret == ESP_OK)
    {
        led_state_ = false;
        ESP_LOGD(TAG, "LED turned off");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to turn off LED: %s", esp_err_to_name(ret));
    }
}

void Tca6408aLed::BlinkOnce()
{
    Blink(1, 100);
}

void Tca6408aLed::Blink(int times, int interval_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经在闪烁，先停止
    if (is_blinking_)
    {
        StopBlinkInternal();
    }

    blink_counter_ = times * 2; // 每次闪烁包含开和关两个动作
    blink_interval_ms_ = interval_ms;
    is_blinking_ = true;

    // 创建定时器（如果尚未创建）
    if (blink_timer_ == nullptr)
    {
        esp_timer_create_args_t timer_args = {
            .callback = OnBlinkTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "tca6408a_blink_timer",
            .skip_unhandled_events = true};

        esp_err_t ret = esp_timer_create(&timer_args, &blink_timer_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create blink timer: %s", esp_err_to_name(ret));
            is_blinking_ = false;
            return;
        }
    }

    // 立即执行第一次动作（点亮）
    HandleBlinkTimer();

    // 启动定时器
    esp_err_t ret = esp_timer_start_periodic(blink_timer_, blink_interval_ms_ * 1000);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start blink timer: %s", esp_err_to_name(ret));
        is_blinking_ = false;
    }
}

void Tca6408aLed::StartContinuousBlink(int interval_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经在闪烁，先停止
    if (is_blinking_)
    {
        StopBlinkInternal();
    }

    blink_counter_ = -1; // -1 表示无限闪烁
    blink_interval_ms_ = interval_ms;
    is_blinking_ = true;

    // 创建定时器（如果尚未创建）
    if (blink_timer_ == nullptr)
    {
        esp_timer_create_args_t timer_args = {
            .callback = OnBlinkTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "tca6408a_blink_timer",
            .skip_unhandled_events = true};

        esp_err_t ret = esp_timer_create(&timer_args, &blink_timer_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create blink timer: %s", esp_err_to_name(ret));
            is_blinking_ = false;
            return;
        }
    }

    // 立即执行第一次动作（点亮）
    HandleBlinkTimer();

    // 启动定时器
    esp_err_t ret = esp_timer_start_periodic(blink_timer_, blink_interval_ms_ * 1000);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start blink timer: %s", esp_err_to_name(ret));
        is_blinking_ = false;
    }
}

void Tca6408aLed::StopBlink()
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopBlinkInternal();
}

void Tca6408aLed::StopBlinkInternal()
{
    if (!is_blinking_)
    {
        return;
    }

    // 停止定时器
    if (blink_timer_ != nullptr)
    {
        esp_timer_stop(blink_timer_);
    }

    is_blinking_ = false;
    blink_counter_ = 0;

    // 熄灭 LED
    {
        std::lock_guard<std::mutex> i2c_lock(tca6408a_i2c_mutex);
        esp_err_t ret = tca6408a_set_gpio_level(tca_handle_, gpio_, 0);
        if (ret == ESP_OK)
        {
            led_state_ = false;
        }
    }

    ESP_LOGD(TAG, "Blink stopped");
}

void Tca6408aLed::HandleBlinkTimer()
{
    // 注意：此函数在定时器回调中调用，已经持有 mutex

    if (!is_blinking_)
    {
        return;
    }

    // 切换 LED 状态
    bool new_state = !led_state_;
    
    {
        std::lock_guard<std::mutex> i2c_lock(tca6408a_i2c_mutex);
        esp_err_t ret = tca6408a_set_gpio_level(tca_handle_, gpio_, new_state ? 1 : 0);

        if (ret == ESP_OK)
        {
            led_state_ = new_state;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to toggle LED: %s", esp_err_to_name(ret));
        }
    }

    // 更新计数器
    if (blink_counter_ > 0)
    {
        blink_counter_--;
        if (blink_counter_ <= 0)
        {
            // 闪烁完成，停止
            is_blinking_ = false;
            esp_timer_stop(blink_timer_);

            // 确保最终状态为熄灭
            {
                std::lock_guard<std::mutex> i2c_lock(tca6408a_i2c_mutex);
                tca6408a_set_gpio_level(tca_handle_, gpio_, 0);
            }
            led_state_ = false;

            ESP_LOGD(TAG, "Blink completed");
        }
    }
    // 如果 blink_counter_ == -1，则持续闪烁
}

void Tca6408aLed::OnBlinkTimer(void *arg)
{
    Tca6408aLed *led = static_cast<Tca6408aLed *>(arg);
    if (led != nullptr)
    {
        std::lock_guard<std::mutex> lock(led->mutex_);
        led->HandleBlinkTimer();
    }
}

void Tca6408aLed::OnStateChanged()
{
    // 根据设备状态控制 LED
}

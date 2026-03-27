#include "tca6408a_led_controller.h"
#include <esp_log.h>
#include <rom/ets_sys.h>

const char *TCA6408ALedController::TAG = "TCA6408ALedController";

/**
 * @brief 构造函数
 */
TCA6408ALedController::TCA6408ALedController() 
    : cold_light_(nullptr),
      warm_light_(nullptr),
      rgb_led_strip_(nullptr),
      num_leds_(0),
      cold_light_state_(false),
      warm_light_state_(false),
      green_ignore_state_(false)
{
    memset(&io_expander_, 0, sizeof(io_expander_));
}

/**
 * @brief 析构函数
 */
TCA6408ALedController::~TCA6408ALedController()
{
    // 清理原生 GPIO LED
    if (cold_light_)
    {
        delete cold_light_;
        cold_light_ = nullptr;
    }

    if (warm_light_)
    {
        delete warm_light_;
        warm_light_ = nullptr;
    }

    // 清理 RGB 灯带
    if (rgb_led_strip_)
    {
        delete rgb_led_strip_;
        rgb_led_strip_ = nullptr;
    }

    // 清理 IO 扩展器
    if (io_expander_.initialized)
    {
        tca6408a_deinit(&io_expander_);
    }
}

/**
 * @brief 初始化 LED 控制器
 */
esp_err_t TCA6408ALedController::Initialize(const led_pin_config_t *pin_config, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address)
{
    if (!pin_config || !i2c_bus)
    {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    pin_config_ = *pin_config;

    // 初始化 TCA6408A（如果尚未初始化）
    if (!io_expander_.initialized)
    {
        tca6408a_config_t io_config = {
            .i2c_bus = i2c_bus,
            .i2c_address = i2c_address,
            .int_gpio = GPIO_NUM_NC,
            .reset_gpio = GPIO_NUM_NC
        };

        esp_err_t ret = tca6408a_init(&io_expander_, &io_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCA6408A: %d", ret);
            return ret;
        }

        ESP_LOGI(TAG, "TCA6408A initialized at 0x%02X", i2c_address);

        // 初始关闭红绿灯
        SetRedLed(false);
        SetGreenLed(false);
    }

    // 初始化冷暖色灯（使用原生 GPIO PWM）
    if (pin_config_->cold_light_gpio >= 0)
    {
        cold_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config_->cold_light_gpio), 
                                  0, LEDC_TIMER_1, LEDC_CHANNEL_3);
        cold_light_->TurnOff();
        ESP_LOGI(TAG, "Cold light initialized on GPIO%d", pin_config_->cold_light_gpio);
    }

    if (pin_config_->warm_light_gpio >= 0)
    {
        warm_light_ = new GpioLed(static_cast<gpio_num_t>(pin_config_->warm_light_gpio), 
                                  0, LEDC_TIMER_1, LEDC_CHANNEL_4);
        warm_light_->TurnOff();
        ESP_LOGI(TAG, "Warm light initialized on GPIO%d", pin_config_->warm_light_gpio);
    }

    // 初始化 RGB 灯带
    if (pin_config_->rgb_gpio >= 0 && pin_config_->rgb_num_leds > 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rgb_led_strip_ = new RgbLedStrip(static_cast<gpio_num_t>(pin_config_->rgb_gpio),
                                         static_cast<uint8_t>(pin_config_->rgb_num_leds));
        num_leds_ = static_cast<uint8_t>(pin_config_->rgb_num_leds);
        ESP_LOGI(TAG, "RGB LED strip initialized with %d LEDs on GPIO%d", 
                 num_leds_, pin_config_->rgb_gpio);
    }

    ESP_LOGI(TAG, "TCA6408A LED Controller initialized successfully");
    return ESP_OK;
}

/**
 * @brief 设置红色 LED
 */
void TCA6408ALedController::SetRedLed(bool on)
{
    if (!io_expander_.initialized)
    {
        return;
    }

    esp_err_t ret = tca6408a_set_gpio_level(&io_expander_, TCA6408A_GPIO_P2, on ? 1 : 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set red LED");
    }
}

/**
 * @brief 设置绿色 LED
 */
void TCA6408ALedController::SetGreenLed(bool on)
{
    if (!io_expander_.initialized)
    {
        return;
    }

    esp_err_t ret = tca6408a_set_gpio_level(&io_expander_, TCA6408A_GPIO_P3, on ? 1 : 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set green LED");
    }
}

/**
 * @brief 启动绿灯呼吸效果
 */
void TCA6408ALedController::StartGreenBreathe()
{
    // IO 扩展器不支持 PWM 呼吸，简化为常亮
    SetGreenLed(true);
}

/**
 * @brief 启动绿灯连续闪烁
 */
void TCA6408ALedController::StartGreenContinuousBlink(int interval_ms)
{
    // IO 扩展器不支持硬件 PWM 闪烁，简化为常亮
    // 如需闪烁需要软件定时器控制
    SetGreenLed(true);
}

/**
 * @brief 更新 LED 状态
 */
void TCA6408ALedController::UpdateLedStatus(DeviceState device_state,
                                            TCA6408APowerManager::PowerState power_state,
                                            TCA6408APowerManager::DevicePowerState device_power_state)
{
    switch (device_power_state)
    {
    case TCA6408APowerManager::DevicePowerState::CHARGING:
        // 充电状态：绿灯灭，红灯根据电源状态显示
        switch (power_state)
        {
        case TCA6408APowerManager::PowerState::USB_POWER_CHARGING:
            SetRedLed(true); // 充电中：红灯常亮（简化）
            break;
        case TCA6408APowerManager::PowerState::USB_POWER_DONE:
            SetRedLed(true); // 充满：红灯常亮
            break;
        case TCA6408APowerManager::PowerState::LOW_BATTERY:
            SetRedLed(true); // 低电量：红灯常亮（简化）
            break;
        default:
            SetRedLed(false);
            break;
        }
        SetGreenLed(false);
        green_ignore_state_ = true;
        break;

    case TCA6408APowerManager::DevicePowerState::POWER_ON:
        // 开机状态：红灯常亮（简化），绿灯根据设备状态显示
        SetRedLed(true);
        
        if (!green_ignore_state_)
        {
            switch (device_state)
            {
            case kDeviceStateIdle:
                StartGreenBreathe(); // 空闲：呼吸（简化为常亮）
                break;
            case kDeviceStateListening:
                SetGreenLed(true); // 监听：常亮
                break;
            case kDeviceStateSpeaking:
                StartGreenContinuousBlink(800); // 说话：闪烁（简化为常亮）
                break;
            case kDeviceStateStarting:
            case kDeviceStateWifiConfiguring:
            case kDeviceStateConnecting:
            case kDeviceStateUpgrading:
            case kDeviceStateActivating:
            case kDeviceStateAudioTesting:
                StartGreenContinuousBlink(200); // 启动等：快闪（简化为常亮）
                break;
            case kDeviceStateFatalError:
                StartGreenContinuousBlink(100); // 错误：极快闪（简化为常亮）
                break;
            default:
                SetGreenLed(false);
                break;
            }
        }
        break;

    case TCA6408APowerManager::DevicePowerState::POWER_OFF:
        // 关机状态：两灯都灭
        SetRedLed(false);
        SetGreenLed(false);
        green_ignore_state_ = true;
        break;

    default:
        break;
    }
}

/**
 * @brief 控制冷色灯
 */
void TCA6408ALedController::SetColdLight(bool state)
{
    if (cold_light_)
    {
        if (state)
        {
            cold_light_->TurnOn();
            cold_light_state_ = true;
        }
        else
        {
            cold_light_->TurnOff();
            cold_light_state_ = false;
        }
    }
}

/**
 * @brief 控制暖色灯
 */
void TCA6408ALedController::SetWarmLight(bool state)
{
    if (warm_light_)
    {
        if (state)
        {
            warm_light_->TurnOn();
            warm_light_state_ = true;
        }
        else
        {
            warm_light_->TurnOff();
            warm_light_state_ = false;
        }
    }
}

/**
 * @brief 设置冷色灯亮度
 */
void TCA6408ALedController::SetColdLightBrightness(int brightness)
{
    if (cold_light_)
    {
        cold_light_->SetBrightness(brightness);
        cold_light_state_ = (brightness > 0);
        if (brightness > 0)
        {
            cold_light_->TurnOn();
        }
        else
        {
            cold_light_->TurnOff();
        }
    }
}

/**
 * @brief 设置暖色灯亮度
 */
void TCA6408ALedController::SetWarmLightBrightness(int brightness)
{
    if (warm_light_)
    {
        warm_light_->SetBrightness(brightness);
        warm_light_state_ = (brightness > 0);
        if (brightness > 0)
        {
            warm_light_->TurnOn();
        }
        else
        {
            warm_light_->TurnOff();
        }
    }
}

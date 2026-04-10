#include "wifi_board.h"
#include "config.h"
#include "tca6408a_io_expander.h"
#include "tca6408a_interrupt_manager.h"
#include "tca6408a_button.h"
#include "tca6408a_power_manager.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#define TAG "FogSeekEdge"

class FogSeekEdge : public WifiBoard
{
private:
    Button boot_button_;
    tca6408a_handle_t tca6408a_handle_;
    TCA6408AInterruptManager *interrupt_manager_;
    TCA6408AButton *ctrl_button_;
    TCA6408APowerManager *power_manager_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t button_monitor_timer_ = nullptr;

    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_GPIO,
            .scl_io_num = I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca6408a()
    {
        tca6408a_config_t tca6408a_config = {
            .i2c_bus = i2c_bus_,
            .i2c_address = 0x20,
            .int_gpio = I2C_INT_GPIO,
            .reset_gpio = GPIO_NUM_NC};

        esp_err_t ret = tca6408a_init(&tca6408a_handle_, &tca6408a_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCA6408A");
            return;
        }

        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_QSPI_PIN_NUM_LCD_BL, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_LED_GREEN_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_LED_RED_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_CTRL_BUTTON_GPIO, TCA6408A_DIR_INPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_HOLD_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_CHARGE_DONE_GPIO, TCA6408A_DIR_INPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_CHARGING_GPIO, TCA6408A_DIR_INPUT);
        // tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_RED_GPIO, 1);
        // tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 1);
        // tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_PWR_HOLD_GPIO, 1);

        ESP_LOGI(TAG, "TCA6408A initialized successfully");
    }

    void InitializeInterruptManager()
    {
        interrupt_manager_ = new TCA6408AInterruptManager(&tca6408a_handle_, I2C_INT_GPIO);
        interrupt_manager_->Initialize();

        ESP_LOGI(TAG, "Interrupt manager initialized on GPIO%d", I2C_INT_GPIO);
    }

    void InitializePowerManager()
    {
        TCA6408APowerManager::power_pin_config_t power_config = {
            .hold_gpio = TCA6408A_PWR_HOLD_GPIO,
            .charging_gpio = TCA6408A_PWR_CHARGING_GPIO,
            .charge_done_gpio = TCA6408A_PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};

        power_manager_ = new TCA6408APowerManager();
        power_manager_->Initialize(&tca6408a_handle_, &power_config);
        ESP_LOGI(TAG, "TCA6408A Power Manager initialized");
    }

    void InitializeCtrlButton()
    {
        ctrl_button_ = new TCA6408AButton(&tca6408a_handle_, TCA6408A_CTRL_BUTTON_GPIO, true);
        ctrl_button_->Initialize(interrupt_manager_);

        ctrl_button_->OnClick([this]()
                              {
            ESP_LOGI(TAG, "Clicked - RED LED on");
            static bool state = false;
            state = !state;
            tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_RED_GPIO, state ? 1 : 0); });

        ctrl_button_->OnDoubleClick([this]()
                                    {
            ESP_LOGI(TAG, "Double clicked - Toggle GREEN LED");
            static bool state = false;
            state = !state;
            tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_GREEN_GPIO, state ? 1 : 0); });

        ctrl_button_->OnLongPress([this]()
                                  {
                                      ESP_LOGI(TAG, "On Long Press");
                                      static bool state = false;
                                      state = !state;
                                      // 切换电源状态
                                      if (state)
                                      {
                                          PowerOn();
                                      }
                                      else
                                      {
                                          PowerOff();
                                      } });
        ESP_LOGI(TAG, "Control button initialized on P%d", TCA6408A_CTRL_BUTTON_GPIO);
    }

    // 定时器回调：每秒打印按键电平状态
    static void ButtonMonitorTimerCallback(void *arg)
    {
        auto instance = static_cast<FogSeekEdge *>(arg);
        uint8_t level;
        esp_err_t ret = tca6408a_get_gpio_level(&instance->tca6408a_handle_, TCA6408A_CTRL_BUTTON_GPIO, &level);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Button P%d level: %d (%s)", TCA6408A_CTRL_BUTTON_GPIO, level, level == 0 ? "PRESSED" : "RELEASED");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to read button level: %d", ret);
        }
    }

    // 初始化按钮监控定时器
    void InitializeButtonMonitor()
    {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = ButtonMonitorTimerCallback;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "button_monitor_timer";
        
        esp_err_t ret = esp_timer_create(&timer_args, &button_monitor_timer_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create button monitor timer: %d", ret);
            return;
        }

        // 启动周期性定时器，间隔1秒（1000000微秒）
        ret = esp_timer_start_periodic(button_monitor_timer_, 1000000);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start button monitor timer: %d", ret);
            esp_timer_delete(button_monitor_timer_);
            button_monitor_timer_ = nullptr;
            return;
        }

        ESP_LOGI(TAG, "Button monitor timer started (interval: 1s)");
    }

    // 处理自动唤醒逻辑
    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            auto &app = Application::GetInstance();
            // USB充电状态下开机需要播放音效
            if (power_manager_->IsUsbPowered())
            {
                app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(500)); // 延时500ms播放音效
            }
            app.Schedule([]()
                         {
                            auto &app = Application::GetInstance();
                            app.ToggleChatState(); });
        }
        else
        {
            // 设备尚未进入空闲状态，500ms后再次检查，使用定时器异步检查，不阻塞当前任务
            esp_timer_handle_t check_timer;
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void *arg)
            {
                auto instance = static_cast<FogSeekEdge *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
        ESP_LOGI(TAG, "Handle Auto Wake.");
    }

    // 开机流程
    void PowerOn()
    {
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_PWR_HOLD_GPIO, 1);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_GREEN_GPIO, 1);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 1);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_PWR_HOLD_GPIO, 0);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 0);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_RED_GPIO, 0);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_LED_GREEN_GPIO, 0);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekEdge() : boot_button_(BOOT_BUTTON_GPIO),
                    interrupt_manager_(nullptr),
                    ctrl_button_(nullptr),
                    power_manager_(nullptr)
    {
        InitializeI2c();
        InitializeTca6408a();
        InitializeInterruptManager();
        InitializePowerManager();
        InitializeCtrlButton();
        InitializeButtonMonitor();
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    ~FogSeekEdge()
    {
        // 停止并删除按钮监控定时器
        if (button_monitor_timer_)
        {
            esp_timer_stop(button_monitor_timer_);
            esp_timer_delete(button_monitor_timer_);
            button_monitor_timer_ = nullptr;
        }

        if (power_manager_)
        {
            delete power_manager_;
        }

        if (ctrl_button_)
        {
            delete ctrl_button_;
        }

        if (interrupt_manager_)
        {
            delete interrupt_manager_;
        }

        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekEdge);
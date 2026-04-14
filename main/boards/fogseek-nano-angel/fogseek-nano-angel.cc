#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "display_manager.h"
#include "motor_controller.h"
#include "codecs/es8389_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include "touch_sensor.h"
#include "boards/lilygo-t-circle-s3/esp_lcd_gc9d01n.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <freertos/task.h>
#include <driver/touch_pad.h>
#include <driver/ledc.h>

#define TAG "FogSeekNanoAngel"

class FogSeekNanoAngel : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    FogSeekMotorController motor_controller_;
    SpiLcdDisplay *display_ = nullptr;
    TouchSensor touch_sensor_; // GPIO44 普通触摸

    // 传感器状态
    bool last_radar_state_ = false;
    bool last_touch_state_ = false;

    // 初始化I2C外设
    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
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

    void InitializeDisplayManager()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 初始化电源管理器
    void InitializePowerManager()
    {
        power_pin_config_t power_pin_config = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    // 初始化LED控制器
    void InitializeLedController()
    {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

    // 初始化音频功放引脚并默认关闭功放
    void InitializeAudioAmplifier()
    {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetAudioAmplifierState(false);
    }

    // 设置音频功放状态
    void SetAudioAmplifierState(bool enable)
    {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    }

    void InitializeMotorController()
    {
        motor_controller_.InitializeMotorPwm(MOTOR_GPIO);
    }

    // 初始化雷达传感器
    void InitializeRadarSensor()
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << RADAR_GPIO);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        ESP_LOGI(TAG, "Radar sensor initialized on GPIO %d", RADAR_GPIO);
    }

    // 读取雷达传感器状态
    bool ReadRadarSensor()
    {
        return gpio_get_level(RADAR_GPIO) == 1;
    }

    // 初始化触摸传感器
    void InitializeTouchSensor()
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << TOUCH_SENSOR_GPIO);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        ESP_LOGI(TAG, "Touch sensor initialized on GPIO%d in level switch mode", TOUCH_SENSOR_GPIO);
    }

    // 读取触摸传感器状态
    bool ReadTouchSensor()
    {
        return gpio_get_level(TOUCH_SENSOR_GPIO) == 1;
    }

    // 传感器监控任务
    static void SensorMonitorTask(void *pvParameters)
    {
        auto instance = static_cast<FogSeekNanoAngel *>(pvParameters);

        ESP_LOGI(TAG, "Sensor monitoring task started");

        while (true)
        {
            // 检测雷达传感器状态变化
            bool radar_state = instance->ReadRadarSensor();
            if (radar_state != instance->last_radar_state_)
            {
                instance->last_radar_state_ = radar_state;
                if (radar_state)
                {
                    // 检测到物体，播放提示音
                    auto &app = Application::GetInstance();
                    app.PlaySound(Lang::Sounds::OGG_WELCOME);
                    ESP_LOGI(TAG, ">>> Radar: Object detected");
                }
                else
                {
                    ESP_LOGI(TAG, ">>> Radar: No object detected");
                }
            }

            // 检测触摸传感器状态变化（电平切换模式）
            bool touch_detected = instance->ReadTouchSensor();

            // 触摸状态发生变化时执行相应操作
            if (touch_detected != instance->last_touch_state_)
            {
                instance->last_touch_state_ = touch_detected;
                // 触摸按下：播放提示音并启动电机（50%占空比）
                auto &app = Application::GetInstance();
                app.PlaySound(Lang::Sounds::OGG_CAT_VOICE01);
                ESP_LOGI(TAG, ">>> Touch PRESSED!");

                if (touch_detected)
                {
                    instance->motor_controller_.SetMotorDutyCycle(50);
                    ESP_LOGI(TAG, ">>> Motor turned ON by Touch (50%% duty cycle)");
                }
                else
                {
                    // 触摸释放：停止电机
                    ESP_LOGI(TAG, ">>> Touch RELEASED");

                    instance->motor_controller_.SetMotorDutyCycle(0);
                    ESP_LOGI(TAG, ">>> Motor turned OFF by Touch");
                }
            }

            // 每100ms轮询一次传感器状态
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // 启动传感器监控
    void StartSensorMonitoring()
    {
        xTaskCreate(SensorMonitorTask, "sensor_monitor", 4096, this, 5, NULL);
        ESP_LOGI(TAG, "Sensor monitoring started");
    }

    // 处理自动唤醒逻辑
    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            auto &app = Application::GetInstance();
            // USB充电状态下开机需要播放音效
            if (power_manager_.IsUsbPowered())
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
                auto instance = static_cast<FogSeekNanoAngel *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
    }

    // 初始化按键回调
    void InitializeButtonCallbacks()
    {
        ctrl_button_.OnClick([this]()
                             {
                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState(); // 切换聊天状态（打断）
                             });
        ctrl_button_.OnDoubleClick([this]()
                                   {
                                        auto &app = Application::GetInstance();
                                        if (app.GetDeviceState() == kDeviceStateStarting)
                                        {
                                            EnterWifiConfigMode();
                                            return;
                                        } });
        ctrl_button_.OnLongPress([this]()
                                 {
            // 切换电源状态
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            } });
    }

    // 开机流程
    void PowerOn()
    {
        power_manager_.PowerOn();                        // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_); // 更新LED灯状态
        // display_manager_.SetBrightness(100);

        // auto codec = GetAudioCodec();
        // codec->SetOutputVolume(70); // 开机后将音量设置为默认值
        // SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "Device powered on.");

        // 启动传感器监控任务
        StartSensorMonitoring();

        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        // display_manager_.SetBrightness(0);

        // auto codec = GetAudioCodec();
        // codec->SetOutputVolume(0); // 关机后将音量设置为默0
        // SetAudioAmplifierState(false);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoAngel() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeDisplayManager();
        // InitializeAudioAmplifier();
        InitializeRadarSensor();
        InitializeTouchSensor();
        InitializeMotorController();
        InitializeButtonCallbacks();

        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static Es8389AudioCodec audio_codec(
            i2c_bus_,
            (i2c_port_t)0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8389_ADDR,
            true,
            true);
        return &audio_codec;
    }
    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
    }

    virtual Display *GetDisplay() override
    {
        return display_;
    }

    ~FogSeekNanoAngel()
    {
        if (check_idle_timer_)
        {
            esp_timer_delete(check_idle_timer_);
        }

        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekNanoAngel);
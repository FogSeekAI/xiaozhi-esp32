#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "display_manager.h"
#include "led_controller.h"
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
#include "mcp_tools.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#include "boards/lilygo-t-circle-s3/esp_lcd_gc9d01n.h"
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include "board.h"
#include "display/lcd_display.h"
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_vendor.h>
#include "backlight.h"
#include "assets.h"

#include "tca6408a_io_expander.h"
#include "tca6408a_interrupt_manager.h"
#include "tca6408a_button.h"
#include "tca6408a_power_manager.h"
#include "esp_lcd_panel_io_additions.h"

#include "../esp_lcd_panel_io_spi_expander/esp_lcd_panel_io_spi_expander.h"

#define TAG "FogseekDualEyesExpand"

class DualDisplayEmotionOnly : public Display {
private:
    SemaphoreHandle_t mutex_ = nullptr; // 添加互斥锁

public:
    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;

    DualDisplayEmotionOnly(SpiLcdDisplay* disp1, SpiLcdDisplay* disp2)
        : display_1_(disp1), display_2_(disp2) {
            mutex_ = xSemaphoreCreateMutex();
            if (!mutex_) {
            ESP_LOGE(TAG, "Failed to create display mutex!");
        }
            // if (display_1_ && display_2_) 
            // {
            //     display_1_->SetTheme(display_2_->GetTheme());
            // }
    }
    ~DualDisplayEmotionOnly() {
        if (mutex_) {
            vSemaphoreDelete(mutex_);
        }
    }
    void SetEmotion(const char* emotion) override {

        if (display_1_) {
            display_1_->SetEmotion(emotion);
            //vTaskDelay(pdMS_TO_TICKS(100)); 
        }
        if (display_2_) {
            display_2_->SetEmotion(emotion);
        }
    }

    void SetTheme(Theme* theme) override {
        if (display_1_) display_1_->SetTheme(theme);
        if (display_2_) display_2_->SetTheme(theme);
    }

public:
    bool Lock(int timeout_ms = 0) override {
        if (!mutex_) return false;
        TickType_t ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xSemaphoreTake(mutex_, ticks) == pdTRUE;
    }

    void Unlock() override {
        if (mutex_) {
            xSemaphoreGive(mutex_);
        }
    }
};
class FogseekDualEyesExpand : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekDisplayManager display_manager_;
    FogSeekLedController led_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    SpiLcdDisplay* display_1=nullptr;
    SpiLcdDisplay* display_2=nullptr;
    DualDisplayEmotionOnly* dual_display_ = nullptr; 

    //Backlight *backlight_1 = new PwmBacklight(DISPLAY_GC9D01_BL_GPIO, true); // true 表示启用 PWM

    tca6408a_handle_t tca6408a_handle_;


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

    void InitializeTca6408a()
    {
        tca6408a_config_t tca6408a_config = {
            .i2c_bus = i2c_bus_,
            .i2c_address = 0x20,
            .reset_gpio = GPIO_NUM_5
        };

        esp_err_t ret = tca6408a_init(&tca6408a_handle_, &tca6408a_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCA6408A");
            return;
        }

        // tca6408a_set_gpio_direction(&tca6408a_handle_, DISPLAY_SPI_CS_1_GPIO, TCA6408A_DIR_OUTPUT);
        // tca6408a_set_gpio_direction(&tca6408a_handle_, DISPLAY_SPI_CS_2_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, DISPLAY_GC9D01_BL_GPIO, TCA6408A_DIR_OUTPUT);
        ESP_LOGI(TAG, "TCA6408A initialized successfully");
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

    // 初始化显示管理器
    void InitializeDisplayManager()
    {
        esp_lcd_panel_io_handle_t panel_io_1 = nullptr;
        esp_lcd_panel_handle_t panel_1 = nullptr;
        esp_lcd_panel_io_handle_t panel_io_2 = nullptr;
        esp_lcd_panel_handle_t panel_2 = nullptr;
        
        spi_bus_config_t buscfg = {};
            buscfg.mosi_io_num = DISPLAY_SPI_MOSI_GPIO;
            buscfg.miso_io_num = GPIO_NUM_NC;
            buscfg.sclk_io_num = DISPLAY_SPI_SCLK_GPIO;
            buscfg.quadwp_io_num = GPIO_NUM_NC;
            buscfg.quadhd_io_num = GPIO_NUM_NC;
            buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_expander_config_t io_config_1 = {
            .pclk_hz = 40 * 1000 * 1000,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_gpio_num = DISPLAY_GC9D01_DC_GPIO,
            .cs_expander_pin = DISPLAY_SPI_CS_1_GPIO,
            .bl_pin = DISPLAY_GC9D01_BL_GPIO,
            .bl_use_expander = true,
            .expander_handle = &tca6408a_handle_,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi_expander(SPI2_HOST, &io_config_1, &panel_io_1));


        esp_lcd_panel_dev_config_t panel_config_1 = {};
            panel_config_1.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
            panel_config_1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
            panel_config_1.bits_per_pixel = 16;
            ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_1, &panel_config_1, &panel_1));
        
        esp_lcd_panel_reset(panel_1);
        esp_lcd_panel_init(panel_1);
        esp_lcd_panel_disp_on_off(panel_1, true);    

        display_1 = new SpiLcdDisplay(panel_io_1, panel_1,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
                                    DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        esp_lcd_panel_io_spi_expander_config_t io_config_2 = {
            .pclk_hz = 40 * 1000 * 1000,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_gpio_num = DISPLAY_GC9D01_DC_GPIO,
            .cs_expander_pin = DISPLAY_SPI_CS_2_GPIO,
            .bl_pin = DISPLAY_GC9D01_BL_GPIO,
            .bl_use_expander = true,
            .expander_handle = &tca6408a_handle_,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi_expander(SPI2_HOST, &io_config_2, &panel_io_2));


        esp_lcd_panel_dev_config_t panel_config_2 = {};
            panel_config_2.reset_gpio_num = GPIO_NUM_NC;//这里需要写空
            panel_config_2.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
            panel_config_2.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_2, &panel_config_2, &panel_2));

        esp_lcd_panel_reset(panel_2);
        esp_lcd_panel_init(panel_2);
        esp_lcd_panel_disp_on_off(panel_2, true);    
        esp_lcd_panel_mirror(panel_2, DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y);

        display_2 = new SpiLcdDisplay(panel_io_2, panel_2, 
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
                                    DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        if (display_1 != nullptr && display_2 != nullptr) {
            dual_display_ = new DualDisplayEmotionOnly(display_1, display_2);
        }
        tca6408a_set_gpio_level(&tca6408a_handle_, DISPLAY_GC9D01_BL_GPIO, 1);
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
        SetAudioAmplifierState(false); // 默认关闭功放
    }

    // 设置音频功放状态
    void SetAudioAmplifierState(bool enable)
    {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
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
                auto instance = static_cast<FogseekDualEyesExpand *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
    }

    // 开机流程
    void PowerOn()
    {
        power_manager_.PowerOn();                        // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_); // 更新LED灯状态
        tca6408a_set_gpio_level(&tca6408a_handle_, DISPLAY_GC9D01_BL_GPIO, 0);
        //display_manager_.SetBrightness(100);
        //gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 0);
        //backlight_1->SetBrightness(100); // 设置 100% 亮度
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(70); // 开机后将音量设置为默认值
        SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        //display_manager_.SetBrightness(0);
        //gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1);
        //backlight_1->SetBrightness(0); // 设置 100% 亮度
        tca6408a_set_gpio_level(&tca6408a_handle_, DISPLAY_GC9D01_BL_GPIO,1);
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(0); // 关机后将音量设置为默0
        SetAudioAmplifierState(false);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }
public:
    FogseekDualEyesExpand() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        //gpio_set_direction(DISPLAY_GC9D01_BL_GPIO, GPIO_MODE_OUTPUT);
        //gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1);
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeTca6408a();
        InitializeDisplayManager();
        InitializeAudioAmplifier();
        InitializeButtonCallbacks();
        
        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual Display *GetDisplay() override
    {
        return dual_display_;
        //return display_2;
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
            GPIO_NUM_NC,
            AUDIO_CODEC_ES8389_ADDR,
            true,
            true);
        return &audio_codec;
    }

    ~FogseekDualEyesExpand()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogseekDualEyesExpand);
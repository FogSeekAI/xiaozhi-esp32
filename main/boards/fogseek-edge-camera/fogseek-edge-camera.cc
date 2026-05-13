#include "wifi_board.h"
#include "config.h"
#include "tca6408a_io_expander.h"
#include "tca6408a_interrupt_manager.h"
#include "tca6408a_button.h"
#include "tca6408a_power_manager.h"
#include "tca6408a_led_controller.h"
#include "tca6408a_led.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include "esp_video.h"
#include "gc2145_camera_driver.h"
#include "esp_lcd_panel_io_spi_expander.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "mcp_server.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9342.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "mcp_server.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <freertos/task.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <driver/i2s_std.h>



#define TAG "FogSeekEdgeCamera"



class FogSeekEdgeCamera : public WifiBoard
{
private:
    Button boot_button_;
    tca6408a_handle_t tca6408a_handle_;
    tca6408a_handle_t tca6408a_second_handle_;
    spi_device_handle_t spi_lcd_ = nullptr;

    Tca6408aInterruptManager interrupt_manager_;
    Tca6408aButton ctrl_button_;
    Tca6408aPowerManager power_manager_;
    Tca6408aLedController led_controller_;
    Tca6408aLed *test_led_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t button_monitor_timer_ = nullptr;
    EspVideo* camera_ = nullptr;
    Gc2145Camera* gc2145_camera_ = nullptr;
    Display* display_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    esp_lcd_panel_handle_t panel_handle = NULL;

    bool camera_initialized_ = false;



    class Tca6408aBacklight : public Backlight {
    private:
        tca6408a_handle_t* tca_handle_;
        tca6408a_gpio_t backlight_pin_;
        
    public:
        Tca6408aBacklight(tca6408a_handle_t* handle, tca6408a_gpio_t pin) 
            : tca_handle_(handle), backlight_pin_(pin) {
        }
        
        void SetBrightnessImpl(uint8_t brightness) override {
            if (tca_handle_) {
                uint8_t level = (brightness > 0) ? 1 : 0;
                ESP_LOGI(TAG, "Setting backlight: brightness=%d, level=%d", brightness, level);
                
                tca6408a_set_gpio_level(tca_handle_, backlight_pin_, level);
            }
        }
    };

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


    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = SPI_PIN_NUM_LCD_MOSI;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = SPI_PIN_NUM_LCD_SCLK;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * (DISPLAY_HEIGHT / 2) * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
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
            ESP_LOGE(TAG, "Failed to initialize Tca6408a (0x20)");
            return;
        }

        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_LED_GREEN_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_LED_RED_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_CTRL_BUTTON_GPIO, TCA6408A_DIR_INPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_HOLD_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_CHARGE_DONE_GPIO, TCA6408A_DIR_INPUT);
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_PWR_CHARGING_GPIO, TCA6408A_DIR_INPUT);
        
        ESP_LOGI(TAG, "Tca6408a (0x20) initialized successfully");
        
        tca6408a_config_t tca6408a_second_config = {
            .i2c_bus = i2c_bus_,
            .i2c_address = TCA6408A_SECOND_ADDR,
            .int_gpio = GPIO_NUM_NC,
            .reset_gpio = GPIO_NUM_NC};

        ret = tca6408a_init(&tca6408a_second_handle_, &tca6408a_second_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize Tca6408a second chip (0x21)");
            return;
        }

        tca6408a_set_gpio_direction(&tca6408a_second_handle_, TCA6408A_SECOND_VBAT_EN_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_second_handle_, TCA6408A_SECOND_LCD_CS_GPIO, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_second_handle_, TCA6408A_SECOND_QSPI_PIN_NUM_LCD_BL, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_direction(&tca6408a_second_handle_, TCA6408A_SECOND_CAM_PWDN_GPIO, TCA6408A_DIR_OUTPUT);
        // 关键修复：立即设置 CS 为高电平（禁用状态），避免悬空
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_LCD_CS_GPIO, 1);
        // 关闭背光
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_QSPI_PIN_NUM_LCD_BL, 0);
        // 使能 VBAT
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_VBAT_EN_GPIO, 1);
        
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_CAM_PWDN_GPIO, 0);
       
        ESP_LOGI(TAG, "Tca6408a second chip (0x21) initialized successfully");
    }

    void InitializeInterruptManager()
    {
        interrupt_manager_.Initialize(&tca6408a_handle_, I2C_INT_GPIO);
        ESP_LOGI(TAG, "Interrupt manager initialized on GPIO%d", I2C_INT_GPIO);
    }

    void InitializePowerManager()
    {
        Tca6408aPowerManager::power_pin_config_t power_config = {
            .hold_gpio = TCA6408A_PWR_HOLD_GPIO,
            .charging_gpio = TCA6408A_PWR_CHARGING_GPIO,
            .charge_done_gpio = TCA6408A_PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};

        power_manager_.Initialize(&tca6408a_handle_, &power_config);
        ESP_LOGI(TAG, "Tca6408a Power Manager initialized");
    }

    void InitializeLedController()
    {
        tca6408a_led_pin_config_t led_config = {
            .red_gpio = TCA6408A_LED_RED_GPIO,
            .green_gpio = TCA6408A_LED_GREEN_GPIO};

        led_controller_.InitializeLeds(&tca6408a_handle_, &led_config, power_manager_);
        ESP_LOGI(TAG, "TCA6408A LED controller initialized");
    }

    void InitializeCtrlButton()
    {
        ctrl_button_.Initialize(&tca6408a_handle_, TCA6408A_CTRL_BUTTON_GPIO, true);
        ctrl_button_.Initialize(&interrupt_manager_);

        ctrl_button_.OnClick([this]()
                             {

                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState();
                                 ESP_LOGI(TAG, "Clicked"); });

        ctrl_button_.OnDoubleClick([this]()
                                   {
                                       
                                       auto &app = Application::GetInstance();
                                       if (app.GetDeviceState() == kDeviceStateStarting)
                                       {
                                           EnterWifiConfigMode();
                                           return;
                                       }
                                       ESP_LOGI(TAG, "Double clicked"); });
        ctrl_button_.OnLongPress([this]()
                                 {
                                      ESP_LOGI(TAG, "On Long Press");
                                      static bool state = false;
                                      state = !state;
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

    void InitializeCamera()
    {
        ESP_LOGI(TAG, "Initializing GC2145 camera using direct SCCB driver...");
        
        // 创建 GC2145 摄像头实例
        gc2145_camera_ = new Gc2145Camera(i2c_bus_);
        
        if (!gc2145_camera_) {
            ESP_LOGE(TAG, "Failed to create GC2145 camera instance");
            return;
        }
        
        // 初始化传感器
        esp_err_t ret = gc2145_camera_->InitSensor();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize GC2145 sensor: %d", ret);
            delete gc2145_camera_;
            gc2145_camera_ = nullptr;
            return;
        }
        
        // 配置分辨率 (QVGA: 320x240)
        ret = gc2145_camera_->ConfigureResolution(320, 240);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure resolution: %d", ret);
            delete gc2145_camera_;
            gc2145_camera_ = nullptr;
            return;
        }
        
        camera_initialized_ = true;
        ESP_LOGI(TAG, "GC2145 camera initialized successfully");
    }

    void EnableLcdCs(bool enable)
    {
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_LCD_CS_GPIO, enable ? 0 : 1);
    }

    
    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Starting LCD initialization");
        
        // 确保背光关闭
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_QSPI_PIN_NUM_LCD_BL, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = SPI_PIN_NUM_LCD_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));
        ESP_LOGI(TAG, "Panel IO installed");

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9342(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "LCD driver installed");
        
        // 拉低 CS 使能 LCD
        ESP_LOGD(TAG, "Setting LCD CS low to enable communication");
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_LCD_CS_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_lcd_panel_reset(panel);
        ESP_LOGI(TAG, "Panel reset");
        
        // 增加复位后的延迟
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_init(panel);
        ESP_LOGI(TAG, "Panel initialized");

        
        
        
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "Panel configuration: swap_xy=%d, mirror_x=%d, mirror_y=%d", 
                 DISPLAY_SWAP_XY, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        
        esp_lcd_panel_disp_on_off(panel, true);
        ESP_LOGI(TAG, "Display turned on");
        vTaskDelay(pdMS_TO_TICKS(50));

        lcd_panel_ = panel;
        lcd_io_ = panel_io;

        

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "EmoteDisplay created");
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "SpiLcdDisplay created");
#endif

        if (display_) {
            ESP_LOGI(TAG, "Display object created successfully");
            ESP_LOGI(TAG, "Display size: %dx%d", display_->width(), display_->height());
        } else {
            ESP_LOGE(TAG, "Failed to create display object!");
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        // 打开背光
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_QSPI_PIN_NUM_LCD_BL, 1);
        ESP_LOGI(TAG, "Backlight turned on");
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "ILI9342 LCD initialization completed");
    }


    
    

    

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) -> std::string {
                EnterWifiConfigMode();
                return "{\"status\": \"entering_wifi_config_mode\"}";
            });

        
    }

    
    

    

    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            auto &app = Application::GetInstance();
            if (power_manager_.IsUsbPowered())
            {
                app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            app.Schedule([]()
                         {
                            auto &app = Application::GetInstance();
                            app.ToggleChatState(); });
        }
        else
        {
            esp_timer_handle_t check_timer;
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void *arg)
            {
                auto instance = static_cast<FogSeekEdgeCamera *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000);
        }
        ESP_LOGI(TAG, "Handle Auto Wake.");
    }

    void PowerOn()
    {
        power_manager_.PowerOn();
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 1);
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_VBAT_EN_GPIO, 1);

        led_controller_.UpdateLedStatus(power_manager_);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake();
    }

    void PowerOff()
    {
        power_manager_.PowerOff();
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 0);
        tca6408a_set_gpio_level(&tca6408a_second_handle_, TCA6408A_SECOND_VBAT_EN_GPIO, 0);

        led_controller_.UpdateLedStatus(power_manager_);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle);
        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekEdgeCamera() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeTca6408a();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeInterruptManager();
        InitializePowerManager();
        InitializeLedController();
        InitializeCtrlButton();
        InitializeCamera();
        InitializeTools();

        if (display_) {
            GetBacklight()->RestoreBrightness();
        }
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

    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
    }

    virtual Camera* GetCamera() override
    {
        return gc2145_camera_;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static Tca6408aBacklight backlight(&tca6408a_second_handle_, TCA6408A_SECOND_QSPI_PIN_NUM_LCD_BL);
        return &backlight;
    }

    ~FogSeekEdgeCamera()
    {
        if (button_monitor_timer_)
        {
            esp_timer_stop(button_monitor_timer_);
            esp_timer_delete(button_monitor_timer_);
            button_monitor_timer_ = nullptr;
        }

        if (lcd_panel_) {
            esp_lcd_panel_del(lcd_panel_);
        }
        if (lcd_io_) {
            esp_lcd_panel_io_del(lcd_io_);
        }
        spi_bus_free(SPI2_HOST);

        if (gc2145_camera_) {
            delete gc2145_camera_;
            gc2145_camera_ = nullptr;
        }

        camera_ = nullptr;

        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekEdgeCamera);
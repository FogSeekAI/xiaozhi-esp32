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
#include <driver/spi_master.h>
#include <freertos/task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io_additions.h>
#include "../esp_lcd_panel_io_spi_expander/esp_lcd_panel_io_spi_expander.h"
#include "board.h"
#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "backlight.h"
#include "assets.h"
#include "boards/lilygo-t-circle-s3/esp_lcd_gc9d01n.h"
#include <sstream>
#include <chrono>


#define TAG "FogSeekNanoToy2"

class DualDisplayEmotionOnly : public Display {
public:
    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;

    DualDisplayEmotionOnly(SpiLcdDisplay* disp1, SpiLcdDisplay* disp2)
        : display_1_(disp1), display_2_(disp2)  
        {
        // 使用实际显示器的尺寸，确保准确性
        if (display_1_) {
            width_ = display_1_->width();
            height_ = display_1_->height();
        } else if (display_2_) {
            width_ = display_2_->width();
            height_ = display_2_->height();
        } else {
        // 如果两个显示器都不可用，使用默认值
        width_ = 160;
        height_ = 160;
    }
        }  
    

    void SetEmotion(const char* emotion) override {
        if (display_1_) {
            display_1_->SetEmotion(emotion);
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
        return lvgl_port_lock(timeout_ms);
    }

    void Unlock() override {
        lvgl_port_unlock();
    }
};
class FogSeekNanoToy2 : public WifiBoard
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

    bool last_touch_state_ = false;
    bool last_radar_state_ = false;
    uint8_t last_battery_level_ = 100;
    static constexpr uint8_t LOW_BATTERY_THRESHOLD = 20;
    
    
    std::chrono::steady_clock::time_point last_touch_trigger_time_;
    std::chrono::steady_clock::time_point last_radar_left_time_;
    bool radar_first_detection_ = true;
    static constexpr int RADAR_DETECTION_INTERVAL_SEC = 180;
    static constexpr int TOUCH_TRIGGER_INTERVAL_SEC = 5;
    
    bool motor_enabled_ = false;
    esp_timer_handle_t motor_timer_ = nullptr;

    EventGroupHandle_t sensor_event_group_ = nullptr;
    static constexpr uint32_t TOUCH_PRESSED_EVENT = BIT0;
    static constexpr uint32_t RADAR_DETECTED_EVENT = BIT1;
    static constexpr uint32_t LOW_BATTERY_EVENT = BIT2;
    static constexpr uint32_t MOTOR_TOGGLE_EVENT = BIT3;

    SpiLcdDisplay* display_1=nullptr;
    SpiLcdDisplay* display_2=nullptr;
    DualDisplayEmotionOnly* dual_display_ = nullptr; 

    TaskHandle_t audio_task_handle_ = nullptr;
    TaskHandle_t motor_task_handle_ = nullptr;

    static constexpr int LOG_BUFFER_SIZE = 256;
    char log_buffer_[LOG_BUFFER_SIZE];
    
    std::chrono::steady_clock::time_point last_motor_trigger_time_;
    bool user_interaction_detected_ = false;
    bool greeting_sent_ = false;
    bool vitality_displayed_ = false;
    static constexpr int GREETING_TIMEOUT_SEC = 60;
    static constexpr int VITALITY_DISPLAY_INTERVAL_SEC = 300;
    
    bool touch_debouncing_ = false;                          
    std::chrono::steady_clock::time_point touch_start_time_; 
    static constexpr int TOUCH_DEBOUNCE_MS = 200;            
    
    bool sleep_warning_shown_ = false;          // 是否已显示休眠警告
    esp_timer_handle_t auto_sleep_timer_ = nullptr; // 自动休眠定时器

    void StartMotorPulse() {
        if (motor_timer_ != nullptr) {
            esp_timer_stop(motor_timer_);
        }
        
        SetMotorState(true);
        motor_enabled_ = true;
        ESP_LOGI(TAG, "Motor pulse started (will auto-stop in 2 seconds)");
        
        esp_timer_start_once(motor_timer_, 2000000);
    }

    static void MotorTimerCallback(void* arg) {
        auto instance = static_cast<FogSeekNanoToy2*>(arg);
        instance->SetMotorState(false);
        instance->motor_enabled_ = false;
        ESP_LOGI(TAG, "Motor pulse completed (auto-stopped)");
    }


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

    void InitializeGpio()
    {
        // 雷达输入
        gpio_config_t radar_cfg = {
            .pin_bit_mask = (1ULL << RADAR_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   // 根据传感器电路决定
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&radar_cfg);

        // 触摸输入
        gpio_config_t touch_cfg = {
            .pin_bit_mask = (1ULL << TOUCH_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&touch_cfg);

        // 电机输出
        gpio_config_t motor_cfg = {
            .pin_bit_mask = (1ULL << MOTOR_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&motor_cfg);
        gpio_set_level(MOTOR_PIN, 0); // 初始关闭

        // 背光输出
        gpio_set_direction(DISPLAY_GC9D01_BL_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1);
    }

    void SetMotorState(bool enable)
    {
        uint8_t level = enable ? 1 : 0;
        gpio_set_level(MOTOR_PIN, level);
        ESP_LOGI(TAG, "Motor state changed: %s", enable ? "ON" : "OFF");
    }

    bool ReadTouchState()
    {
        return gpio_get_level(TOUCH_PIN) != 0;
    }
    bool ReadRadarState()
    {
        return gpio_get_level(RADAR_PIN) != 0;
    }


    void InitializePowerManager()
    {
        power_pin_config_t power_pin_config = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    void InitializeLedController()
    {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

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

    void SetAudioAmplifierState(bool enable)
    {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    }

    void InitializeButtonCallbacks()
    {
        ctrl_button_.OnClick([this]()
                             {
                                //  auto codec = GetAudioCodec();
                                //  int current_volume = codec->output_volume();
                                 gpio_set_level(MOTOR_PIN, 1);
                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState(); // 切换聊天状态（打断）
                            
                             });
        ctrl_button_.OnDoubleClick([this]()
                                   {
            gpio_set_level(MOTOR_PIN, 0);
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting)
            {
                EnterWifiConfigMode();
                return;
            } });
        ctrl_button_.OnLongPress([this]()
                                 {
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            } });
    }



    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.toy.set_motor",
            "Control the toy's motor (vibration motor). Use this to turn the motor on or off.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                SetMotorState(enabled);
                motor_enabled_ = enabled;
                
                ESP_LOGI(TAG, "Motor controlled via voice: %s", enabled ? "ON" : "OFF");
                return true;
            });
        
        mcp_server.AddTool("self.toy.get_motor_status",
            "Get the current status of the toy's motor.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "motor_enabled", motor_enabled_);
                cJSON_AddStringToObject(json, "status", motor_enabled_ ? "on" : "off");
                return json;
            });
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

        esp_lcd_panel_io_spi_config_t io_config_1 = {};
            io_config_1.cs_gpio_num = DISPLAY_SPI_CS_1_GPIO;
            io_config_1.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
            io_config_1.spi_mode = 0;
            io_config_1.pclk_hz = 40* 1000 * 1000;
            io_config_1.trans_queue_depth = 10;
            io_config_1.lcd_cmd_bits = 8;
            io_config_1.lcd_param_bits = 8;
        
        esp_lcd_panel_dev_config_t panel_config_1 = {};
            panel_config_1.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
            panel_config_1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
            panel_config_1.bits_per_pixel = 16;
            ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config_1, &panel_io_1));

            ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_1, &panel_config_1, &panel_1));
            esp_lcd_panel_reset(panel_1);
            esp_lcd_panel_init(panel_1);
            esp_lcd_panel_disp_on_off(panel_1, true);    

        display_1 = new SpiLcdDisplay(panel_io_1, panel_1,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
                                    DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        esp_lcd_panel_io_spi_config_t io_config_2 = {};
            io_config_2.cs_gpio_num = DISPLAY_SPI_CS_2_GPIO;
            io_config_2.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
            io_config_2.spi_mode = 0;
            io_config_2.pclk_hz = 40 * 1000 * 1000;
            io_config_2.trans_queue_depth = 10;
            io_config_2.lcd_cmd_bits = 8;
            io_config_2.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config_2, &panel_io_2));

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
    }


     static void SensorMonitorTask(void *pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy2 *>(pvParameters);
        
        ESP_LOGI(TAG, "=== SensorMonitorTask STARTED ===");
        
        uint32_t loop_count = 0;
        uint32_t battery_check_counter = 0;
        const uint32_t BATTERY_CHECK_INTERVAL = 600;
        
        while (true) {
            loop_count++;
            bool current_touch = instance->ReadTouchState();
            if (current_touch) {
                if (!instance->touch_debouncing_) {
                    instance->touch_debouncing_ = true;
                    instance->touch_start_time_ = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - instance->touch_start_time_).count();
                    
                    if (duration_ms >= TOUCH_DEBOUNCE_MS && !instance->last_touch_state_) {
                        instance->last_touch_state_ = true;
                        ESP_LOGI(TAG, "[#%lu] Valid touch detected (held for %ld ms)", 
                                (unsigned long)loop_count, (long)duration_ms);
                        
                        auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(
                            now - instance->last_touch_trigger_time_).count();
                        if (time_since_last >= instance->TOUCH_TRIGGER_INTERVAL_SEC) {
                            xEventGroupSetBits(instance->sensor_event_group_, 
                                            TOUCH_PRESSED_EVENT | MOTOR_TOGGLE_EVENT);
                            instance->last_touch_trigger_time_ = now;
                            ESP_LOGI(TAG, ">>> Touch Event Sent (interval: %ld sec) <<<", 
                                    (long)time_since_last);
                        } else {
                            ESP_LOGI(TAG, "Touch ignored (too soon: %ld sec)", 
                                    (long)time_since_last);
                        }
                        }
                    }
                } else {
                    instance->touch_debouncing_ = false;
                    if (instance->last_touch_state_) {
                        instance->last_touch_state_ = false;
                        ESP_LOGI(TAG, "[#%lu] Touch released", (unsigned long)loop_count);
                    }
                }
            
            bool radar_detected = instance->ReadRadarState();
            if (radar_detected != instance->last_radar_state_) {
                instance->last_radar_state_ = radar_detected;
                ESP_LOGI(TAG, "[#%lu] Radar CHANGED to: %d", 
                        (unsigned long)loop_count, radar_detected ? 1 : 0);
                if (radar_detected) {
                    xEventGroupSetBits(instance->sensor_event_group_, RADAR_DETECTED_EVENT);
                    ESP_LOGI(TAG, ">>> Radar Person Detected <<<");
                } else {
                    instance->last_radar_left_time_ = std::chrono::steady_clock::now();
                    ESP_LOGI(TAG, ">>> Radar Person Left - Timer Reset <<<");
                }
            }
            
            if (loop_count <= 20 || loop_count % 100 == 0) {
                bool radar_now = instance->ReadRadarState();
                bool touch_now = instance->ReadTouchState();
                
                ESP_LOGI(TAG, "[#%lu] Radar=%d Touch=%d | Motor=%s", 
                        (unsigned long)loop_count,
                        radar_now ? 1 : 0,
                        touch_now ? 1 : 0,
                        instance->motor_enabled_ ? "ON" : "OFF");
            }

            if (++battery_check_counter >= BATTERY_CHECK_INTERVAL) {
                battery_check_counter = 0;
                uint8_t current_level = instance->power_manager_.ReadBatteryLevel();
                if (current_level < instance->LOW_BATTERY_THRESHOLD && 
                    instance->last_battery_level_ >= instance->LOW_BATTERY_THRESHOLD) {
                    xEventGroupSetBits(instance->sensor_event_group_, instance->LOW_BATTERY_EVENT);
                    ESP_LOGW(TAG, "Low battery: %d%%", current_level);
                }
                instance->last_battery_level_ = current_level;
            }
            
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }


      static void AudioTask(void* pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy2*>(pvParameters);
        uint32_t events;
        
        while (true) {
            events = xEventGroupWaitBits(instance->sensor_event_group_, 
                                         instance->TOUCH_PRESSED_EVENT,
                                         pdTRUE,
                                         pdFALSE,
                                         portMAX_DELAY);
            
            if (events & instance->TOUCH_PRESSED_EVENT) {
                ESP_LOGI(TAG, "AudioTask: Touch detected, will trigger AI");
                auto& app = Application::GetInstance();
                
                DeviceState current_state = app.GetDeviceState();
                ESP_LOGI(TAG, "Current device state: %d", static_cast<int>(current_state));
                
                std::string wake_word = "我摸你头";
                
                if (current_state == DeviceState::kDeviceStateListening) {
                    ESP_LOGI(TAG, "Device is listening, need to close session first");
                    
                    app.WakeWordInvoke(wake_word);
                    ESP_LOGI(TAG, "First wake word sent to close listening session");
                    
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    
                    DeviceState new_state = app.GetDeviceState();
                    ESP_LOGI(TAG, "After delay, device state: %d", static_cast<int>(new_state));
                    
                    if (new_state == DeviceState::kDeviceStateIdle) {
                        app.WakeWordInvoke(wake_word);
                        ESP_LOGI(TAG, "Second wake word sent to activate AI");
                    } else {
                        ESP_LOGW(TAG, "State not idle after delay (%d), using Schedule", static_cast<int>(new_state));
                        app.Schedule([wake_word]() {
                            auto& app = Application::GetInstance();
                            app.WakeWordInvoke(wake_word);
                        });
                    }
                } else if (current_state == DeviceState::kDeviceStateIdle || 
                           current_state == DeviceState::kDeviceStateSpeaking) {
                    
                    app.WakeWordInvoke(wake_word);
                    ESP_LOGI(TAG, "Sent wake word: %s, state was acceptable", wake_word.c_str());
                } else {
                    ESP_LOGW(TAG, "Device state %d not suitable for wake word, scheduling retry", 
                            static_cast<int>(current_state));
                    
                    app.Schedule([wake_word]() {
                        auto& app = Application::GetInstance();
                        ESP_LOGI(TAG, "Scheduled wake word invoke: %s", wake_word.c_str());
                        app.WakeWordInvoke(wake_word);
                    });
                }
            }
        }
    }

    static void MotorTask(void* pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy2*>(pvParameters);
        uint32_t events;
        
        while (true) {
            events = xEventGroupWaitBits(instance->sensor_event_group_, 
                                         instance->MOTOR_TOGGLE_EVENT | instance->RADAR_DETECTED_EVENT | instance->LOW_BATTERY_EVENT,
                                         pdTRUE,
                                         pdFALSE,
                                         pdMS_TO_TICKS(10));
            
            if (events & instance->MOTOR_TOGGLE_EVENT) {
                instance->last_motor_trigger_time_ = std::chrono::steady_clock::now();
                
                if (!instance->user_interaction_detected_) {
                    instance->user_interaction_detected_ = true;
                    instance->greeting_sent_ = false;
                    instance->vitality_displayed_ = false;
                    ESP_LOGI(TAG, "MotorTask: User interaction detected, reset state variables");
                } else {
                    ESP_LOGI(TAG, "MotorTask: Motor pulse triggered by touch/radar, timer reset");
                }
                
                instance->StartMotorPulse();
                ESP_LOGI(TAG, "MotorTask: Motor pulse triggered by sensor event");
            }
            
            else if (events & instance->RADAR_DETECTED_EVENT) {
                bool should_notify = false;
                
                if (instance->radar_first_detection_) {
                    should_notify = true;
                    instance->radar_first_detection_ = false;
                    ESP_LOGI(TAG, "MotorTask: First radar detection, will notify AI");
                } else {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - instance->last_radar_left_time_).count();
                    
                    if (elapsed >= instance->RADAR_DETECTION_INTERVAL_SEC) {
                        should_notify = true;
                        ESP_LOGI(TAG, "MotorTask: Radar detected, interval=%ld sec (>= %d sec), will notify AI", 
                                (long)elapsed, instance->RADAR_DETECTION_INTERVAL_SEC);
                    } else {
                        ESP_LOGI(TAG, "MotorTask: Radar detected but interval=%ld sec (< %d sec), skipped", 
                                (long)elapsed, instance->RADAR_DETECTION_INTERVAL_SEC);
                    }
                }
                
                if (should_notify) {
                    instance->last_motor_trigger_time_ = std::chrono::steady_clock::now();
                    instance->user_interaction_detected_ = true;
                    instance->greeting_sent_ = false;
                    instance->vitality_displayed_ = false;
                    ESP_LOGI(TAG, "MotorTask: Radar detected person, reset state variables");
                    
                    instance->StartMotorPulse();
                    ESP_LOGI(TAG, "MotorTask: Radar detected person, triggering AI and motor pulse");
                    auto& app = Application::GetInstance();
                    
                    DeviceState current_state = app.GetDeviceState();
                    ESP_LOGI(TAG, "Current device state for radar: %d", static_cast<int>(current_state));
                    
                    std::string wake_word = "主人来了";
                    
                    if (current_state == DeviceState::kDeviceStateListening) {
                        ESP_LOGI(TAG, "Device is listening, need to close session first");
                        
                        app.WakeWordInvoke(wake_word);
                        ESP_LOGI(TAG, "First wake word sent to close listening session");
                        
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        
                        DeviceState new_state = app.GetDeviceState();
                        ESP_LOGI(TAG, "After delay, device state: %d", static_cast<int>(new_state));
                        
                        if (new_state == DeviceState::kDeviceStateIdle) {
                            app.WakeWordInvoke(wake_word);
                            ESP_LOGI(TAG, "Second wake word sent to activate AI");
                        } else {
                            ESP_LOGW(TAG, "State not idle after delay (%d), using Schedule", static_cast<int>(new_state));
                            app.Schedule([wake_word]() {
                                auto& app = Application::GetInstance();
                                app.WakeWordInvoke(wake_word);
                            });
                        }
                    } else if (current_state == DeviceState::kDeviceStateIdle || 
                               current_state == DeviceState::kDeviceStateSpeaking) {
                        
                        app.WakeWordInvoke(wake_word);
                        ESP_LOGI(TAG, "Sent radar wake word: %s, state was acceptable", wake_word.c_str());
                    } else {
                        ESP_LOGW(TAG, "Device state %d not suitable for radar wake word, scheduling retry", 
                                static_cast<int>(current_state));
                        
                        app.Schedule([wake_word]() {
                            auto& app = Application::GetInstance();
                            ESP_LOGI(TAG, "Scheduled radar wake word invoke: %s", wake_word.c_str());
                            app.WakeWordInvoke(wake_word);
                        });
                    }
                }
            }
            else if (events & instance->LOW_BATTERY_EVENT) {
                ESP_LOGW(TAG, "MotorTask: Low battery warning");
                auto& app = Application::GetInstance();
                
                DeviceState current_state = app.GetDeviceState();
                ESP_LOGI(TAG, "Current device state for low battery: %d", static_cast<int>(current_state));
                
                std::string wake_word = "低电量";
                
                if (current_state == DeviceState::kDeviceStateListening) {
                    ESP_LOGI(TAG, "Device is listening, need to close session first");
                    
                    app.WakeWordInvoke(wake_word);
                    ESP_LOGI(TAG, "First wake word sent to close listening session");
                    
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    
                    DeviceState new_state = app.GetDeviceState();
                    ESP_LOGI(TAG, "After delay, device state: %d", static_cast<int>(new_state));
                    
                    if (new_state == DeviceState::kDeviceStateIdle) {
                        app.WakeWordInvoke(wake_word);
                        ESP_LOGW(TAG, "Second wake word sent for low battery alert");
                    } else {
                        ESP_LOGW(TAG, "State not idle after delay (%d), using Schedule", static_cast<int>(new_state));
                        app.Schedule([wake_word]() {
                            auto& app = Application::GetInstance();
                            app.WakeWordInvoke(wake_word);
                        });
                    }
                } else if (current_state == DeviceState::kDeviceStateIdle || 
                           current_state == DeviceState::kDeviceStateSpeaking) {
                    
                    app.WakeWordInvoke(wake_word);
                    ESP_LOGW(TAG, "Sent low battery wake word: %s, state was acceptable", wake_word.c_str());
                } else {
                    ESP_LOGW(TAG, "Device state %d not suitable for low battery wake word, scheduling retry", 
                            static_cast<int>(current_state));
                    
                    app.Schedule([wake_word]() {
                        auto& app = Application::GetInstance();
                        ESP_LOGI(TAG, "Scheduled low battery wake word invoke: %s", wake_word.c_str());
                        app.WakeWordInvoke(wake_word);
                    });
                }
            }
            
            auto now = std::chrono::steady_clock::now();
            auto elapsed_since_last_trigger = std::chrono::duration_cast<std::chrono::seconds>(
                now - instance->last_motor_trigger_time_).count();
            
        
            if (instance->user_interaction_detected_ && instance->greeting_sent_ && 
                !instance->vitality_displayed_ && 
                elapsed_since_last_trigger >= instance->VITALITY_DISPLAY_INTERVAL_SEC) {
                
                instance->vitality_displayed_ = true;
                instance->last_motor_trigger_time_ = std::chrono::steady_clock::now();
                
                ESP_LOGI(TAG, "MotorTask: Displaying vitality after %ld sec, timer reset but state unchanged", 
                        (long)elapsed_since_last_trigger);
                
                instance->StartMotorPulse();
            }
            
            if (instance->user_interaction_detected_ && instance->vitality_displayed_ && 
                elapsed_since_last_trigger >= instance->VITALITY_DISPLAY_INTERVAL_SEC) {
                
                instance->vitality_displayed_ = false;
                instance->last_motor_trigger_time_ = std::chrono::steady_clock::now();
                
                ESP_LOGI(TAG, "MotorTask: Periodic vitality display after %ld sec", 
                        (long)elapsed_since_last_trigger);
                
                instance->StartMotorPulse();
            }
        }
    }
    
     void StartSensorMonitoring()
    {
        ESP_LOGI(TAG, "Starting sensor monitoring...");
        
        last_touch_state_ = ReadTouchState();
        last_radar_state_ = ReadRadarState();
        motor_enabled_ = false;
        
        last_radar_left_time_ = std::chrono::steady_clock::now();
        last_touch_trigger_time_ = std::chrono::steady_clock::now();
        last_motor_trigger_time_ = std::chrono::steady_clock::now();
        
        radar_first_detection_ = true;
        user_interaction_detected_ = false;
        greeting_sent_ = false;
        vitality_displayed_ = false;
        
        ESP_LOGI(TAG, "Initial states - Touch: %d, Radar: %d, Motor: OFF, UserInteraction: false", 
                last_touch_state_ ? 1 : 0, last_radar_state_ ? 1 : 0);
        
        esp_timer_create_args_t motor_timer_args = {};
        motor_timer_args.callback = MotorTimerCallback;
        motor_timer_args.arg = this;
        motor_timer_args.name = "motor_timer";
        motor_timer_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&motor_timer_args, &motor_timer_);
        
        sensor_event_group_ = xEventGroupCreate();
        
        xTaskCreate(AudioTask, "audio_task", 4096, this, 3, &audio_task_handle_);
        ESP_LOGI(TAG, "Audio task created");
        
        xTaskCreate(MotorTask, "motor_task", 4096, this, 2, &motor_task_handle_);
        ESP_LOGI(TAG, "Motor task created");
        
        xTaskCreate(SensorMonitorTask, "sensor_monitor", 4096, this, 1, NULL);
        ESP_LOGI(TAG, "Sensor monitor task created");
        
        ESP_LOGI(TAG, "All sensor tasks started");
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
                auto instance = static_cast<FogSeekNanoToy2 *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000);
        }
    }

    void PowerOn()
    {
        power_manager_.PowerOn();
        led_controller_.UpdateLedStatus(power_manager_);
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(70); // 开机后将音量设置为默认值
        ESP_LOGI(TAG, "Device powered on.");
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 0);
         if (dual_display_ && dual_display_->display_1_ && dual_display_->display_2_) {
        dual_display_->display_1_->SetTheme(dual_display_->display_2_->GetTheme());
        }
        //task_flag = true;
        StartSensorMonitoring();
        HandleAutoWake();
    }

    void PowerOff()
    {
        SetMotorState(false);
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1); 
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(0); // 关机后将音量设置为默0
        gpio_set_level(MOTOR_PIN, 0); 
        //task_flag = false;
        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle);

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoToy2() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeGpio();
        InitializePowerManager();
        InitializeLedController();
        InitializeAudioAmplifier();
        InitializeDisplayManager();
        InitializeButtonCallbacks();
        InitializeTools();
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual Display *GetDisplay() override
    {
        return dual_display_;
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

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override
    {
        auto power_state = power_manager_.GetPowerState();
        
        level = power_manager_.ReadBatteryLevel();
        
        charging = (power_state == FogSeekPowerManager::PowerState::USB_POWER_CHARGING);
        
        discharging = (power_state == FogSeekPowerManager::PowerState::BATTERY_POWER || 
                      power_state == FogSeekPowerManager::PowerState::LOW_BATTERY);
        
            ESP_LOGD(TAG, "Battery: level=%d%%, charging=%d, discharging=%d, state=%d", 
                 level, charging, discharging, static_cast<int>(power_state));
        
        return true;
    }

    ~FogSeekNanoToy2()
    {
        if (check_idle_timer_) {
            esp_timer_delete(check_idle_timer_);
        }

        if (motor_timer_) {
            esp_timer_stop(motor_timer_);
            esp_timer_delete(motor_timer_);
        }
        
        if (sensor_event_group_) {
            vEventGroupDelete(sensor_event_group_);
        }
        
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
  
    }
};
//bool FogSeekNanoToy2::task_flag = false;
DECLARE_BOARD(FogSeekNanoToy2);
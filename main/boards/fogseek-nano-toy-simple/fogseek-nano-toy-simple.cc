#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "button.h"
#include "board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/lvgl_image.h"
#include "boards/lilygo-t-circle-s3/esp_lcd_gc9d01n.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "ToySimple"

// ─── 嵌入式 GIF 资源 (放在 boards/fogseek-nano-toy-simple/ 目录) ────
extern const char _binary_emoji1_gif_start[] asm("_binary_emoji1_gif_start");
extern const char _binary_emoji1_gif_end[]   asm("_binary_emoji1_gif_end");
extern const char _binary_emoji2_gif_start[] asm("_binary_emoji2_gif_start");
extern const char _binary_emoji2_gif_end[]   asm("_binary_emoji2_gif_end");

// ─── 双屏显示器包装类 (同 toy-2) ───────────────────────────

class DualDisplayEmotionOnly : public Display {
public:
    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;

    DualDisplayEmotionOnly(SpiLcdDisplay* disp1, SpiLcdDisplay* disp2)
        : display_1_(disp1), display_2_(disp2) {
        if (display_1_) {
            width_ = display_1_->width();
            height_ = display_1_->height();
        } else if (display_2_) {
            width_ = display_2_->width();
            height_ = display_2_->height();
        } else {
            width_ = 160;
            height_ = 160;
        }
    }

    void SetEmotion(const char* emotion) override {
        if (display_1_) display_1_->SetEmotion(emotion);
        if (display_2_) display_2_->SetEmotion(emotion);
    }

    void SetTheme(Theme* theme) override {
        if (display_1_) display_1_->SetTheme(theme);
        if (display_2_) display_2_->SetTheme(theme);
    }

    bool Lock(int timeout_ms = 0) override {
        return lvgl_port_lock(timeout_ms);
    }
    void Unlock() override {
        lvgl_port_unlock();
    }
};

// ─── 最简板卡类 ─────────────────────────────────────────

class FogSeekNanoToySimple : public WifiBoard {
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;

    esp_timer_handle_t motor_timer_ = nullptr;
    esp_timer_handle_t emoji_timer_ = nullptr;
    bool motor_enabled_ = false;

    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;
    DualDisplayEmotionOnly* dual_display_ = nullptr;

    static constexpr const char* EMOJI_IDLE = "idle";
    static constexpr const char* EMOJI_ACTIVE = "active";

    // 触摸消抖
    bool touch_debouncing_ = false;
    int64_t touch_start_ms_ = 0;
    static constexpr int TOUCH_DEBOUNCE_MS = 200;

    // 触摸冷却（5秒内最多触发一次）
    int64_t last_touch_time_ms_ = 0;
    static constexpr int TOUCH_COOLDOWN_MS = 5000;

    // ── 电机控制 ─────────────────────────────────────

    void SetMotorState(bool enable) {
        gpio_set_level(MOTOR_PIN, enable ? 1 : 0);
        ESP_LOGI(TAG, "Motor: %s", enable ? "ON" : "OFF");
    }

    void StartMotorPulse() {
        if (motor_timer_) esp_timer_stop(motor_timer_);
        SetMotorState(true);
        motor_enabled_ = true;
        esp_timer_start_once(motor_timer_, 2000000); // 2秒
    }

    static void MotorTimerCallback(void* arg) {
        auto* self = static_cast<FogSeekNanoToySimple*>(arg);
        self->SetMotorState(false);
        self->motor_enabled_ = false;
    }

    static void EmojiTimerCallback(void* arg) {
        auto* self = static_cast<FogSeekNanoToySimple*>(arg);
        if (self->dual_display_) {
            self->dual_display_->SetEmotion(EMOJI_IDLE);
        }
    }

    // ── GPIO ─────────────────────────────────────────

    void InitializeGpio() {
        gpio_config_t touch_cfg = {
            .pin_bit_mask = 1ULL << TOUCH_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&touch_cfg);

        gpio_config_t motor_cfg = {
            .pin_bit_mask = 1ULL << MOTOR_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&motor_cfg);
        gpio_set_level(MOTOR_PIN, 0);

        gpio_set_direction(DISPLAY_GC9D01_BL_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1);
    }

    bool ReadTouchState() {
        return gpio_get_level(TOUCH_PIN) != 0;
    }

    // ── 显示器初始化 (同 toy-2) ───────────────────────

    void InitializeDisplayManager() {
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

        // 左眼 (CS1)
        esp_lcd_panel_io_spi_config_t io_config_1 = {};
        io_config_1.cs_gpio_num = DISPLAY_SPI_CS_1_GPIO;
        io_config_1.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
        io_config_1.spi_mode = 0;
        io_config_1.pclk_hz = 40 * 1000 * 1000;
        io_config_1.trans_queue_depth = 10;
        io_config_1.lcd_cmd_bits = 8;
        io_config_1.lcd_param_bits = 8;

        esp_lcd_panel_dev_config_t panel_config_1 = {};
        panel_config_1.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
        panel_config_1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config_1.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config_1, &panel_io_1));
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(
            panel_io_1, &panel_config_1, &panel_1));
        esp_lcd_panel_reset(panel_1);
        esp_lcd_panel_init(panel_1);
        esp_lcd_panel_disp_on_off(panel_1, true);

        display_1_ = new SpiLcdDisplay(panel_io_1, panel_1,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        // 右眼 (CS2)
        esp_lcd_panel_io_spi_config_t io_config_2 = {};
        io_config_2.cs_gpio_num = DISPLAY_SPI_CS_2_GPIO;
        io_config_2.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
        io_config_2.spi_mode = 0;
        io_config_2.pclk_hz = 40 * 1000 * 1000;
        io_config_2.trans_queue_depth = 10;
        io_config_2.lcd_cmd_bits = 8;
        io_config_2.lcd_param_bits = 8;

        esp_lcd_panel_dev_config_t panel_config_2 = {};
        panel_config_2.reset_gpio_num = GPIO_NUM_NC;
        panel_config_2.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config_2.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            SPI2_HOST, &io_config_2, &panel_io_2));
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(
            panel_io_2, &panel_config_2, &panel_2));
        esp_lcd_panel_reset(panel_2);
        esp_lcd_panel_init(panel_2);
        esp_lcd_panel_disp_on_off(panel_2, true);
        esp_lcd_panel_mirror(panel_2, DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y);

        display_2_ = new SpiLcdDisplay(panel_io_2, panel_2,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        if (display_1_ && display_2_) {
            dual_display_ = new DualDisplayEmotionOnly(display_1_, display_2_);
        }
    }

    // ── 自定义 EmojiCollection (使用本地 GIF) ──────────

    void InitializeCustomEmojis() {
        auto custom_emojis = std::make_shared<EmojiCollection>();

        size_t size;
        size = _binary_emoji1_gif_end - _binary_emoji1_gif_start;
        custom_emojis->AddEmoji("idle",
            new LvglRawImage((void*)_binary_emoji1_gif_start, size));

        size = _binary_emoji2_gif_end - _binary_emoji2_gif_start;
        custom_emojis->AddEmoji("active",
            new LvglRawImage((void*)_binary_emoji2_gif_start, size));

        // 注册到两个 Display 的 Theme 上
        if (display_1_) {
            auto* theme = static_cast<LvglTheme*>(display_1_->GetTheme());
            theme->set_emoji_collection(custom_emojis);
        }
        if (display_2_ && display_2_->GetTheme() != display_1_->GetTheme()) {
            auto* theme = static_cast<LvglTheme*>(display_2_->GetTheme());
            theme->set_emoji_collection(custom_emojis);
        }

        ESP_LOGI(TAG, "Custom emoji collection registered (idle + active)");
    }

    // ── 电源管理 ─────────────────────────────────────

    void InitializePowerManager() {
        power_pin_config_t cfg = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO,
        };
        power_manager_.Initialize(&cfg);
    }

    void InitializeLedController() {
        led_pin_config_t cfg = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO,
        };
        led_controller_.InitializeLeds(power_manager_, &cfg);
    }

    void PowerOn() {
        power_manager_.PowerOn();
        led_controller_.UpdateLedStatus(power_manager_);
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 0); // 开背光
        if (dual_display_ && dual_display_->display_1_ && dual_display_->display_2_) {
            dual_display_->display_1_->SetTheme(dual_display_->display_2_->GetTheme());
        }
        StartSensorTask();
        ESP_LOGI(TAG, "Powered ON");
    }

    void PowerOff() {
        SetMotorState(false);
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        gpio_set_level(DISPLAY_GC9D01_BL_GPIO, 1); // 关背光
        gpio_set_level(MOTOR_PIN, 0);
        ESP_LOGI(TAG, "Powered OFF");
    }

    // ── 按钮回调 ─────────────────────────────────────

    void InitializeButtonCallbacks() {
        ctrl_button_.OnLongPress([this]() {
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            }
        });
    }

    // ── 触摸检测任务 ─────────────────────────────────

    void StartSensorTask() {
        esp_timer_create_args_t motor_args = {};
        motor_args.callback = MotorTimerCallback;
        motor_args.arg = this;
        motor_args.name = "motor_timer";
        motor_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&motor_args, &motor_timer_);

        esp_timer_create_args_t emoji_args = {};
        emoji_args.callback = EmojiTimerCallback;
        emoji_args.arg = this;
        emoji_args.name = "emoji_timer";
        emoji_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&emoji_args, &emoji_timer_);

        xTaskCreate(SensorTask, "sensor", 4096, this, 2, nullptr);
        ESP_LOGI(TAG, "Sensor task started");
    }

    static void SensorTask(void* pvParameters) {
        auto* self = static_cast<FogSeekNanoToySimple*>(pvParameters);
        int64_t now = 0;
        bool last_touch = false;

        while (true) {
            bool touch = self->ReadTouchState();

            // 冷却期内忽略触摸
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - self->last_touch_time_ms_ < TOUCH_COOLDOWN_MS) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (touch && !self->touch_debouncing_) {
                self->touch_debouncing_ = true;
                self->touch_start_ms_ = esp_timer_get_time() / 1000;
            }

            if (self->touch_debouncing_) {
                now = esp_timer_get_time() / 1000;
                if ((now - self->touch_start_ms_) >= TOUCH_DEBOUNCE_MS) {
                    // 消抖完成，有效触摸
                    if (touch && !last_touch) {
                        ESP_LOGI(TAG, "Touch detected → active emoji + motor");

                        self->last_touch_time_ms_ = now;

                        // 切换到 active 表情
                        if (self->dual_display_) {
                            self->dual_display_->SetEmotion(EMOJI_ACTIVE);
                        }

                        // 电机振动
                        self->StartMotorPulse();

                        // 3秒后自动切回 idle
                        esp_timer_stop(self->emoji_timer_);
                        esp_timer_start_once(self->emoji_timer_, 3000000);

                        last_touch = true;
                    }
                    if (!touch) {
                        self->touch_debouncing_ = false;
                        last_touch = false;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

public:
    FogSeekNanoToySimple()
        : boot_button_(BOOT_BUTTON_GPIO),
          ctrl_button_(CTRL_BUTTON_GPIO) {
        InitializeGpio();
        InitializePowerManager();
        InitializeLedController();
        InitializeDisplayManager();
        InitializeCustomEmojis();
        InitializeButtonCallbacks();
        power_manager_.SetPowerStateCallback(
            [this](FogSeekPowerManager::PowerState) {
                led_controller_.UpdateLedStatus(power_manager_);
            });

        // 开机即显示默认情绪
        if (dual_display_) {
            dual_display_->SetEmotion(EMOJI_IDLE);
        }
    }

    ~FogSeekNanoToySimple() {
        if (motor_timer_) {
            esp_timer_stop(motor_timer_);
            esp_timer_delete(motor_timer_);
        }
        if (emoji_timer_) {
            esp_timer_stop(emoji_timer_);
            esp_timer_delete(emoji_timer_);
        }
    }

    // ── Board 接口实现 ───────────────────────────────

    void StartNetwork() override {
        // Offline board: do nothing, no WiFi
        ESP_LOGI(TAG, "Offline mode: network disabled");
    }

    Display* GetDisplay() override { return dual_display_; }
    Led* GetLed() override { return led_controller_.GetGreenLed(); }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        auto state = power_manager_.GetPowerState();
        level = power_manager_.ReadBatteryLevel();
        charging = (state == FogSeekPowerManager::PowerState::USB_POWER_CHARGING);
        discharging = (state == FogSeekPowerManager::PowerState::BATTERY_POWER ||
                       state == FogSeekPowerManager::PowerState::LOW_BATTERY);
        return true;
    }
};

DECLARE_BOARD(FogSeekNanoToySimple);

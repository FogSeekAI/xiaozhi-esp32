#include "motor_controller.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#define TAG "FogSeekMotorController"

FogSeekMotorController::FogSeekMotorController() : servo_gpio_(GPIO_NUM_NC), 
                                                   channel_(LEDC_CHANNEL_0), 
                                                   timer_(LEDC_TIMER_0), 
                                                   current_angle_(90), 
                                                   initialized_(false), 
                                                   motor_gpio_(GPIO_NUM_NC),
                                                   motor_initialized_(false), 
                                                   motor_timer_handle_(nullptr),
                                                   run_time_ms_(0) {}

FogSeekMotorController::~FogSeekMotorController()
{
    if (initialized_)
    {
        ledc_stop(LEDC_LOW_SPEED_MODE, channel_, 0);
    }
    
    if (motor_timer_handle_ != nullptr) {
        esp_timer_delete(motor_timer_handle_);
    }
}

void FogSeekMotorController::InitializeServo(gpio_num_t servo_gpio)
{
    servo_gpio_ = servo_gpio;

    // 配置LEDC定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT, // 13位分辨率
        .timer_num = timer_,
        .freq_hz = 50, // 50Hz PWM频率，周期20ms
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置LEDC通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = servo_gpio_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer_,
        .duty = 0,
        .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // 设置初始角度
    SetAngle(current_angle_);
    initialized_ = true;

    ESP_LOGI(TAG, "Servo controller initialized on GPIO %d", servo_gpio_);
}

void FogSeekMotorController::SetAngle(uint16_t angle)
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "Servo controller not initialized");
        return;
    }

    // 限制角度范围
    if (angle > 180)
    {
        angle = 180;
    }

    current_angle_ = angle;

    // 计算PWM占空比
    // 通常舵机的控制脉冲范围是500-2500微秒，对应0-180度
    // 对应LEDC的duty值约为262-1310 (基于13位分辨率和20ms周期)
    uint32_t duty = (uint32_t)(((angle / 180.0) * (1310 - 262)) + 262);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_));
}

uint16_t FogSeekMotorController::GetAngle() const
{
    return current_angle_;
}

void FogSeekMotorController::InitializeMotor(gpio_num_t motor_gpio)
{
    motor_gpio_ = motor_gpio;
    
    // 配置GPIO为输出模式
    // gpio_config_t io_conf = {};
    // io_conf.intr_type = GPIO_INTR_DISABLE;
    // io_conf.mode = GPIO_MODE_OUTPUT;
    // io_conf.pin_bit_mask = (1ULL << motor_gpio_);
    // io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    // io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    // gpio_config(&io_conf);
    
    // // 默认设置为低电平（停止）
    // gpio_set_level(motor_gpio_, 0);
    
    motor_initialized_ = true;
    
    // ESP_LOGI(TAG, "Motor controller initialized on GPIO %d", motor_gpio_);
}

void FogSeekMotorController::ControlMotor(bool state)
{
    if (!motor_initialized_)
    {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return;
    }
    
    gpio_set_level(motor_gpio_, state ? 1 : 0);
    ESP_LOGD(TAG, "Motor state set to %s", state ? "ON" : "OFF");
}

void FogSeekMotorController::RunMotorTimed(uint32_t run_time_ms)
{
    if (!motor_initialized_)
    {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return;
    }
    
    // 如果已有定时器正在运行，先删除它
    if (motor_timer_handle_ != nullptr) {
        esp_timer_stop(motor_timer_handle_);
        esp_timer_delete(motor_timer_handle_);
        motor_timer_handle_ = nullptr;
    }
    
    // 启动电机
    ControlMotor(true);
    
    // 创建一个ESP-IDF定时器来停止电机
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void* arg) {
        FogSeekMotorController* motor_ctrl = static_cast<FogSeekMotorController*>(arg);
        motor_ctrl->ControlMotor(false);  // 停止电机
        ESP_LOGI(TAG, "Motor stopped after timed run");
    };
    timer_args.arg = this;
    timer_args.name = "motor_timer";
    
    esp_err_t err = esp_timer_create(&timer_args, &motor_timer_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create motor timer");
        ControlMotor(false); // 立即停止电机
        return;
    }
    
    err = esp_timer_start_once(motor_timer_handle_, run_time_ms * 1000); // 转换为微秒
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start motor timer");
        ControlMotor(false); // 立即停止电机
    } else {
        ESP_LOGI(TAG, "Motor started for %d ms (single run, no loop)", run_time_ms);
    }
}

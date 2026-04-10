#ifndef _MOTOR_CONTROLLER_H_
#define _MOTOR_CONTROLLER_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

class FogSeekMotorController
{
public:
    FogSeekMotorController();
    ~FogSeekMotorController();

    // 舵机控制相关方法
    // 初始化舵机控制器
    void InitializeServo(gpio_num_t servo_gpio);

    // 设置舵机角度 (0-180度)
    void SetServoAngle(uint16_t angle);

    // 获取当前角度
    uint16_t GetServoAngle() const;

    // 电机控制相关方法
    // 初始化电机控制器
    void InitializeIOMotor(gpio_num_t motor_gpio);

    // 直接控制电机状态
    void ControlIOMotor(bool state);

    // 定时运行电机，运行指定时间后停止
    void RunIOMotorTimed(uint32_t run_time_ms);

    void InitializeMotorPwm(gpio_num_t motor_gpio);
    void SetMotorDutyCycle(uint8_t percentage);
    void IncreaseMotorDutyCycle(uint8_t increment);

private:
    // 舵机相关属性
    gpio_num_t servo_gpio_;
    ledc_channel_t channel_;
    ledc_timer_t timer_;
    uint16_t current_angle_;
    bool initialized_;

    // 电机相关属性
    gpio_num_t motor_gpio_;
    bool motor_initialized_;
    esp_timer_handle_t motor_timer_handle_;

    // 用于存储定时参数
    uint32_t run_time_ms_;

    // PWM 配置
    bool pwm_initialized_ = false;
    bool motor_enabled_ = false;    // 电机开关状态
    uint32_t motor_duty_cycle_ = 0; // 当前占空比（0-4095）
};

#endif // _MOTOR_CONTROLLER_H_
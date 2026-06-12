#ifndef _MOTOR_CONTROLLER_H_
#define _MOTOR_CONTROLLER_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <map>
#include <string>

enum ServoId {
    SERVO_ID_1 = 0,  // 第一个舵机（例如：IO3）
    SERVO_ID_2 = 1,  // 第二个舵机（例如：IO39）
    SERVO_MAX
};

struct ServoConfig {
    gpio_num_t gpio;
    ledc_channel_t channel;
    uint16_t current_angle;
    bool initialized;
    
    // 校准参数
    uint32_t min_duty;  // 0度对应的duty值
    uint32_t max_duty;  // 180度对应的duty值
    
    ServoConfig() : gpio(GPIO_NUM_NC), channel(LEDC_CHANNEL_0), 
                    current_angle(90), initialized(false),
                    min_duty(205), max_duty(1024) {}
};

class FogSeekMotorController
{
public:
    FogSeekMotorController();
    ~FogSeekMotorController();

    // 舵机控制相关方法
    // 初始化指定ID的舵机
    void InitializeServo(ServoId id, gpio_num_t servo_gpio);
    
    // 初始化指定ID的舵机，带校准参数
    void InitializeServo(ServoId id, gpio_num_t servo_gpio, uint32_t min_duty, uint32_t max_duty);

    // 设置舵机角度 (0-180度)
    void SetAngle(uint16_t angle);

    // 获取当前角度
    uint16_t GetAngle() const;

    // 电机控制相关方法
    // 初始化电机控制器
    void InitializeMotor(gpio_num_t motor_gpio);

    // 直接控制电机状态
    void ControlMotor(bool state);

    // 定时运行电机，运行指定时间后停止
    void RunMotorTimed(uint32_t run_time_ms);

    void InitializeMotor(gpio_num_t motor_gpio);
    void SetMotorDutyCycle(uint8_t percentage);
    void IncreaseMotorDutyCycle(uint8_t increment);

private:
    // 舵机相关属性
    std::map<ServoId, ServoConfig> servos_;
    ledc_timer_t timer_;
    bool timer_initialized_;

    // 电机相关属性
    gpio_num_t motor_gpio_;
    bool motor_initialized_;
    esp_timer_handle_t motor_timer_handle_;

    // 用于存储定时参数
    uint32_t run_time_ms_;
};

#endif // _MOTOR_CONTROLLER_H_
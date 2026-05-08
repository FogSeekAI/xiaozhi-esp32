#ifndef _FRAGRANCE_CONTROLLER_H_
#define _FRAGRANCE_CONTROLLER_H_

#include "led_controller.h"
#include "motor_controller.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#define FRAGRANCE_TAG "FragranceController"

class FragranceController
{
public:
    enum class Mode
    {
        NORMAL_MODE,
        WORK_MODE,
        SLEEP_AID_MODE,
        STRESS_RELIEF_MODE,
        OFF_MODE
    };

    FragranceController(FogSeekLedController &led_ctrl, FogSeekMotorController &motor_ctrl);
    ~FragranceController();

    void SetMode(Mode mode);
    void SetIntensityHigh(); // 高浓度模式
    void SetIntensityLow();  // 低浓度模式
    void TurnOff();          // 关闭香氛
    void SetNormalModeParams();
private:
    Mode current_mode_ = Mode::OFF_MODE;
    bool is_running_ = false;

    FogSeekLedController &led_controller_;
    FogSeekMotorController &motor_controller_;

    // 定时器句柄
    esp_timer_handle_t cycle_timer_ = nullptr;

    // 模式特定参数
    uint32_t active_duration_ = 0; // 活跃阶段持续时间
    uint32_t cycle_duration_ = 0;  // 整个周期持续时间

    void SetWorkModeParams();
    void SetSleepAidModeParams();
    void SetStressReliefModeParams();
    

    void StartCycleTimer();
    void StopCycleTimer();
    void CycleTimerCallback();
    void StopCurrentMode();

};

#endif // _FRAGRANCE_CONTROLLER_H_d
#include "fragrance_controller.h"
#include "led_controller.h"

FragranceController::FragranceController(FogSeekLedController &led_ctrl, FogSeekMotorController &motor_ctrl)
    : led_controller_(led_ctrl), motor_controller_(motor_ctrl) {}

FragranceController::~FragranceController()
{
    StopCurrentMode();
}

void FragranceController::SetMode(Mode mode)
{
    // 停止当前模式
    StopCurrentMode();

    current_mode_ = mode;

    if (mode != Mode::OFF_MODE)
    {
        // 根据模式设置参数
        switch (mode)
        {
        case Mode::NORMAL_MODE:
            SetNormalModeParams();
            break;
        case Mode::WORK_MODE:
            SetWorkModeParams();
            led_controller_.SetAllLightsLowBrightness();
            break;
        case Mode::SLEEP_AID_MODE:
            SetSleepAidModeParams();
            led_controller_.StartBreathingEffect(1500);
            StartSleepModeStopTimer();
            break;
        case Mode::STRESS_RELIEF_MODE:
            SetStressReliefModeParams();
            led_controller_.StartBreathingEffect(4000);
            break;
        default:
            break;
        }
        motor_controller_.RunMotorTimed(active_duration_);
        // 启动模式循环
        StartCycleTimer();
        is_running_ = true;
    }
    else
    {
        is_running_ = false;
        motor_controller_.ControlMotor(false);
        led_controller_.TurnOffRgbLights(1000);
    }
}

void FragranceController::SetIntensityHigh()
{
    // 停止当前模式
    StopCurrentMode();

    // 设置高浓度参数：运行10秒
    active_duration_ = 10000; // 10秒

    // 使用电机控制器的定时功能运行电机
    motor_controller_.RunMotorTimed(active_duration_);

    ESP_LOGI(FRAGRANCE_TAG, "Fragrance intensity set to HIGH: run %d ms", active_duration_);
}

void FragranceController::SetIntensityLow()
{
    // 停止当前模式
    StopCurrentMode();

    // 设置低浓度参数：运行3秒
    active_duration_ = 3000; // 3秒

    // 使用电机控制器的定时功能运行电机
    motor_controller_.RunMotorTimed(active_duration_);

    ESP_LOGI(FRAGRANCE_TAG, "Fragrance intensity set to LOW: run %d ms", active_duration_);
}

void FragranceController::TurnOff()
{
    StopCurrentMode();
    SetMode(Mode::OFF_MODE);
    ESP_LOGI(FRAGRANCE_TAG, "Fragrance turned OFF");
}

void FragranceController::StartCycleTimer()
{
    if (cycle_timer_ != nullptr)
    {
        esp_timer_stop(cycle_timer_);
        esp_timer_delete(cycle_timer_);
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void *arg)
    {
        auto controller = static_cast<FragranceController *>(arg);
        controller->CycleTimerCallback();
    };
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "fragrance_cycle_timer";

    esp_err_t err = esp_timer_create(&timer_args, &cycle_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(FRAGRANCE_TAG, "Failed to create cycle timer: %s", esp_err_to_name(err));
        return;
    }

    // 立即启动第一个周期
    esp_timer_start_periodic(cycle_timer_, cycle_duration_ * 1000);
}

void FragranceController::StopCycleTimer()
{
    if (cycle_timer_ != nullptr)
    {
        esp_timer_stop(cycle_timer_);
        esp_timer_delete(cycle_timer_);
        cycle_timer_ = nullptr;
    }
}

void FragranceController::CycleTimerCallback()
{
    if (!is_running_)
        return;

    switch (current_mode_)
    {
        case Mode::NORMAL_MODE:
            motor_controller_.ControlMotor(true);
            break;
        case Mode::WORK_MODE:
            motor_controller_.RunMotorTimed(active_duration_);
            break;

        case Mode::SLEEP_AID_MODE:
            motor_controller_.RunMotorTimed(active_duration_);
            break;

        case Mode::STRESS_RELIEF_MODE:
            motor_controller_.RunMotorTimed(active_duration_);
            break;

        case Mode::OFF_MODE:
            motor_controller_.ControlMotor(false);
            break;

    default:
        break;
    }
}

void FragranceController::StopCurrentMode()
{
    StopCycleTimer();
    StopSleepModeStopTimer();

    led_controller_.TurnOffRgbLights(100);

    // 停止电机
    motor_controller_.ControlMotor(false);
}

void FragranceController::SetWorkModeParams()//工作模式
{
    active_duration_ = 10000; // 10秒活跃时间
    cycle_duration_ = 30000; // 30秒周期
    //motor_controller_.RunMotorTimed(active_duration_);
    ESP_LOGI(FRAGRANCE_TAG, "Work mode activated: motor runs for %d ms every %d ms cycle",
             active_duration_, cycle_duration_);
}

void FragranceController::SetSleepAidModeParams()//助眠模式
{
    active_duration_ = 10000;   //10秒活跃时间
    cycle_duration_ = 70000;  //70秒周期
    //motor_controller_.RunMotorTimed(active_duration_);
    ESP_LOGI(FRAGRANCE_TAG, "Sleep aid mode activated: motor runs for %d ms every %d ms cycle",
             active_duration_, cycle_duration_);
}

void FragranceController::SetStressReliefModeParams()//解压模式
{
    active_duration_ = 10000;  // 5秒活跃时间
    cycle_duration_ = 50000; // 40秒周期
    //motor_controller_.RunMotorTimed(active_duration_);
    ESP_LOGI(FRAGRANCE_TAG, "Stress relief mode activated: motor runs for %d ms every %d ms cycle",
             active_duration_, cycle_duration_);
}
void FragranceController::SetNormalModeParams()
{
        
        led_controller_.SetAllLightsLowBrightness();
        ESP_LOGI(FRAGRANCE_TAG, "Motor and lights turned ON");
}
static void SleepModeStopTimerCallback(void *arg)
{
    auto controller = static_cast<FragranceController *>(arg);
    ESP_LOGI(FRAGRANCE_TAG, "Sleep mode timeout after 30 minutes, turning off fragrance");
    controller->SetMode(FragranceController::Mode::OFF_MODE);
}

// 新增：启动助眠模式停止定时器
void FragranceController::StartSleepModeStopTimer()
{
    if (current_mode_ != Mode::SLEEP_AID_MODE) {
        return;
    }
    
    if (sleep_mode_stop_timer_ != nullptr)
    {
        esp_timer_stop(sleep_mode_stop_timer_);
        esp_timer_delete(sleep_mode_stop_timer_);
        sleep_mode_stop_timer_ = nullptr;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = SleepModeStopTimerCallback; 
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "sleep_mode_stop_timer";

    esp_err_t err = esp_timer_create(&timer_args, &sleep_mode_stop_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(FRAGRANCE_TAG, "Failed to create sleep mode stop timer: %s", esp_err_to_name(err));
        return;
    }

    esp_timer_start_once(sleep_mode_stop_timer_, 1800000000ULL); 
    ESP_LOGI(FRAGRANCE_TAG, "Sleep mode stop timer started: will stop after 30 minutes");
}

// 新增：停止助眠模式停止定时器
void FragranceController::StopSleepModeStopTimer()
{
    if (sleep_mode_stop_timer_ != nullptr)
    {
        esp_timer_stop(sleep_mode_stop_timer_);
        esp_timer_delete(sleep_mode_stop_timer_);
        sleep_mode_stop_timer_ = nullptr;
    }
}
# 雾岸科技 Nano - Angel 项目（陈总娃头）

## 产品概述

Angel 是一款基于 Nano 核心板和 Toy1_V1.1 定制拓展板开发的智能互动玩具设备（陈总娃头）。该设备集成了双眼屏幕、雷达传感器、多模式触摸和电机翅膀等丰富交互模块，通过智能化的传感器融合和动作反馈，为用户提供生动有趣的人机交互体验。

### 核心特性
- 🎭 **双眼表情显示**：双 GC9D01N SPI 屏幕同步显示，呈现丰富的表情动画
- 📡 **人体检测**：雷达传感器实时监测人体存在，实现自动唤醒
- 👆 **双模触摸交互**：GPIO44 按压触摸 + GPIO9 电容触摸，多种互动方式
- 🦋 **电机翅膀控制**：PWM 精确控制电机转速，根据触摸输入动态调整翅膀摆动
- 🔊 **音频交互**：ES8389 高性能音频编解码器，支持音效播放和语音对话
- 💡 **状态指示**：RGB LED 实时反馈设备运行状态
- 🔋 **智能电源管理**：电池监控、充电状态检测、自动开关机

---

## 硬件组成

### 核心硬件
- **主控芯片**: ESP32-S3（Nano 核心板）
- **存储配置**: 16MB Flash + 8MB PSRAM
- **定制拓展板**: Toy1_V1.1

### 显示系统
- **显示屏**: 2x GC9D01N SPI LCD（240x240 分辨率）
- **显示接口**: SPI2_HOST 总线共享
- **颜色格式**: RGB565 (16-bit)，BGR 元素顺序

### 传感器系统
- **雷达传感器**: 数字输出，高电平表示检测到物体
- **触摸传感器 1**: GPIO44 普通 GPIO 触摸（按压式）
- **触摸传感器 2**: GPIO9 ESP32 内置电容触摸

### 执行器系统
- **电机翅膀**: LEDC PWM 控制（5kHz，12位分辨率）
- **LED 指示**: RGB LED（红色 + 绿色通道）

### 音频系统
- **编解码器**: ES8389（I2C 地址 0x10）
- **功放控制**: 独立 GPIO 控制音频功放开关
- **音频接口**: I2S 标准接口（MCLK/BCLK/WS/DOUT/DIN）

### 交互按键
- **启动按键**: BOOT_BUTTON_GPIO
- **控制按键**: CTRL_BUTTON_GPIO（单击/双击/长按多功能）

### 电源管理
- **电池监控**: ADC 采样电池电压
- **充电检测**: 充电状态和充满状态独立 GPIO
- **电源保持**: PWR_HOLD_GPIO 控制电源开关

---

## 软件架构

### 项目结构
```
fogseek-nano-angel/
├── fogseek-nano-angel.cc    # 主控制类实现
└── config.h                  # 引脚配置和参数定义
```

### 核心类设计

#### FogSeekNanoAngel 类
继承自 `WifiBoard` 基类，实现完整的设备控制逻辑。

**主要成员变量**:
- `boot_button_`: 启动按键对象
- `ctrl_button_`: 控制按键对象
- `power_manager_`: 电源管理器实例
- `led_controller_`: LED 控制器实例
- `touch_sensor_1_`: GPIO44 普通触摸传感器
- `touch_sensor_2_`: GPIO9 电容触摸传感器
- `audio_codec_`: ES8389 音频编解码器
- `i2c_bus_`: I2C 总线句柄

**关键配置参数** (需在 `config.h` 中定义):

```cpp
// ========== 显示屏配置 ==========
#define DISPLAY_SPI_MOSI_GPIO      xx
#define DISPLAY_SPI_SCLK_GPIO      xx
#define DISPLAY_SPI_CS_GPIO        xx
#define DISPLAY_GC9D01_DC_GPIO     xx
#define DISPLAY_GC9D01_RESET_GPIO  xx
#define DISPLAY_WIDTH              240
#define DISPLAY_HEIGHT             240
#define DISPLAY_OFFSET_X           0
#define DISPLAY_OFFSET_Y           0
#define DISPLAY_MIRROR_X           false
#define DISPLAY_MIRROR_Y           false
#define DISPLAY_SWAP_XY            false

// ========== 传感器配置 ==========
#define RADAR_GPIO                 xx
#define TOUCH_SENSOR_1_GPIO        44
#define TOUCH_SENSOR_2_CHANNEL     TOUCH_PAD_NUM9
#define TOUCH_SENSOR_2_THRESHOLD_PERCENT 70

// ========== 电机配置 ==========
#define MOTOR_GPIO                 xx

// ========== 音频配置 ==========
#define AUDIO_CODEC_I2C_SDA_PIN    xx
#define AUDIO_CODEC_I2C_SCL_PIN    xx
#define AUDIO_CODEC_PA_PIN         xx
#define AUDIO_I2S_GPIO_MCLK        xx
#define AUDIO_I2S_GPIO_BCLK        xx
#define AUDIO_I2S_GPIO_WS          xx
#define AUDIO_I2S_GPIO_DOUT        xx
#define AUDIO_I2S_GPIO_DIN         xx
#define AUDIO_CODEC_ES8389_ADDR    0x10

// ========== 电源管理配置 ==========
#define PWR_HOLD_GPIO              xx
#define PWR_CHARGING_GPIO          xx
#define PWR_CHARGE_DONE_GPIO       xx
#define BATTERY_ADC_GPIO           xx

// ========== LED 配置 ==========
#define LED_RED_GPIO               xx
#define LED_GREEN_GPIO             xx

// ========== 按键配置 ==========
#define BOOT_BUTTON_GPIO           xx
#define CTRL_BUTTON_GPIO           xx
```

### 初始化流程

构造函数按以下顺序执行初始化：

1. **InitializeI2c()**: 初始化 I2C 总线（用于音频编解码器通信）
2. **InitializePowerManager()**: 初始化电源管理器（配置电池监控引脚）
3. **InitializeLedController()**: 初始化 LED 控制器（配置 RGB LED 引脚）
4. **InitializeAudioAmplifier()**: 初始化音频功放引脚（默认关闭）
5. **InitializeButtonCallbacks()**: 设置按键回调函数
6. **注册电源状态回调**: 自动更新 LED 状态

---

## 功能模块详解

### 一、双眼屏幕显示

#### 功能说明
`InitializeDisplayManager()` 函数负责初始化双屏显示系统。两个屏幕共用 SPI 总线，显示相同内容，实现"双眼"效果。

#### 实现要点

```cpp
void InitializeDisplayManager()
{
    esp_log_level_set("lcd", ESP_LOG_DEBUG);
    
    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 配置 SPI 总线
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_SPI_MOSI_GPIO;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_SPI_SCLK_GPIO;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 配置第一个屏幕的 SPI IO
    esp_lcd_panel_io_spi_config_t io_config_1 = {};
    io_config_1.cs_gpio_num = DISPLAY_SPI_CS_GPIO;
    io_config_1.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
    io_config_1.spi_mode = 0;
    io_config_1.pclk_hz = 40 * 1000 * 1000;  // 40MHz 时钟
    io_config_1.trans_queue_depth = 10;
    io_config_1.lcd_cmd_bits = 8;
    io_config_1.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config_1, &panel_io_1));

    // 配置 GC9D01N 面板驱动
    esp_lcd_panel_dev_config_t panel_config_1 = {};
    panel_config_1.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
    panel_config_1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config_1.bits_per_pixel = 16;  // RGB565 格式
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_1, &panel_config_1, &panel_1));

    // 复位并初始化面板
    esp_lcd_panel_reset(panel_1);
    esp_lcd_panel_init(panel_1);
    esp_lcd_panel_disp_on_off(panel_1, true);  // 开启显示

    // 创建显示对象
    display_1 = new SpiLcdDisplay(panel_io_1, panel_1,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                  DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                  DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, 
                                  DISPLAY_SWAP_XY);
}
```

#### 技术要点
- **SPI 总线共享**: 两个屏幕共用 SPI2_HOST，通过不同 CS 引脚选择
- **GC9D01N 驱动**: 使用定制的 `esp_lcd_gc9d01n` 驱动，支持 240x240 分辨率
- **显示同步**: 通过 `SpiLcdDisplay` 类统一管理，确保双屏一致
- **颜色格式**: RGB565 (16-bit)，BGR 元素顺序

#### ⚠️ 注意事项
当前代码仅初始化了一个屏幕。如需真正的双屏显示，需要：
1. 定义第二个屏幕的 CS 和 DC 引脚
2. 创建第二组 `panel_io_2` 和 `panel_2`
3. 初始化第二个 `SpiLcdDisplay` 对象
4. 渲染时同时更新两个显示对象

---

### 二、雷达传感器

#### 功能说明
`InitializeRadarSensor()` 初始化雷达传感器，用于检测人体存在。雷达模块输出数字信号，高电平表示检测到物体。

#### 实现代码

```cpp
void InitializeRadarSensor()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << RADAR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;  // ✅ 已修正为输入模式
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;  // ✅ 启用下拉电阻
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Radar sensor initialized on GPIO %d", RADAR_GPIO);
}

bool ReadRadarSensor()
{
    return gpio_get_level(RADAR_GPIO) == 1;
}
```

#### 技术要点
- **GPIO 配置**: 配置为输入模式，启用下拉电阻确保稳定电平
- **轮询方式**: 在 `SensorMonitorTask` 中每 100ms 读取一次状态
- **状态变化检测**: 仅在状态变化时输出日志，减少冗余信息

---

### 三、触摸传感器

#### 功能说明
系统支持两种触摸传感器，提供多样化的交互方式：
- **Touch Sensor 1 (GPIO44)**: 普通 GPIO 触摸，通过电平判断触摸状态
- **Touch Sensor 2 (GPIO9)**: ESP32 电容触摸，使用内置触摸外设

#### 传感器监控任务

```cpp
static void SensorMonitorTask(void *pvParameters)
{
    auto instance = static_cast<FogSeekNanoAngel *>(pvParameters);
    ESP_LOGI(TAG, "Sensor monitoring task started");

    while (true)
    {
        // 1. 读取雷达传感器
        bool radar_state = instance->ReadRadarSensor();
        if (radar_state != instance->last_radar_state_)
        {
            instance->last_radar_state_ = radar_state;
            ESP_LOGI(TAG, radar_state ? ">>> Radar: Object DETECTED!" : 
                                      ">>> Radar: No object detected");
        }

        // 2. 读取 GPIO44 普通触摸传感器
        int touch1_level = gpio_get_level(TOUCH_SENSOR_1_GPIO);
        bool touch1_detected = (touch1_level == 1);

        if (touch1_detected != instance->last_touch1_state_)
        {
            instance->last_touch1_state_ = touch1_detected;
            if (touch1_detected)
            {
                ESP_LOGI(TAG, ">>> Touch 1 (GPIO%d): PRESSED!", TOUCH_SENSOR_1_GPIO);
                // 播放小狗叫声
                auto &app = Application::GetInstance();
                app.PlaySound(Lang::Sounds::OGG_DOG_VOICE03);
                
                // ✅ 开启电机（50% 占空比）
                instance->SetMotorDutyCycle(50);
                ESP_LOGI(TAG, ">>> Motor turned ON by Touch 1 (50%% duty cycle)");
            }
            else
            {
                ESP_LOGI(TAG, ">>> Touch 1 (GPIO%d): RELEASED", TOUCH_SENSOR_1_GPIO);
                // ✅ 关闭电机（已修正为 0）
                instance->SetMotorDutyCycle(0);
                ESP_LOGI(TAG, ">>> Motor turned OFF by Touch 1");
            }
        }

        // 3. 读取 GPIO9 电容触摸传感器
        uint32_t touch2_value = instance->touch_sensor_2_.ReadCapTouchValue();
        int32_t touch2_delta = (int32_t)touch2_value - (int32_t)instance->touch_sensor_2_.GetBaseline();
        bool touch2_detected = instance->touch_sensor_2_.IsCapTouchDetected();

        if (touch2_detected != instance->last_touch2_state_)
        {
            instance->last_touch2_state_ = touch2_detected;
            if (touch2_detected)
            {
                ESP_LOGI(TAG, ">>> Touch 2 (GPIO9): TOUCHED! Value: %" PRIu32 ", Delta: %" PRId32,
                         touch2_value, touch2_delta);
                // 增加占空比 10%
                instance->IncreaseMotorDutyCycle(10);
                ESP_LOGI(TAG, ">>> Motor speed increased by Touch 2");
            }
            else
            {
                ESP_LOGI(TAG, ">>> Touch 2 (GPIO9): RELEASED");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms 采样周期
    }
}
```

#### 技术要点

**Touch Sensor 1 (GPIO44)**:
- 使用普通 GPIO 输入方式
- 高电平表示触摸按下
- 按下时播放音效并启动电机（50% 占空比）
- 释放时停止电机（0% 占空比）

**Touch Sensor 2 (GPIO9)**:
- 使用 ESP32 内置电容触摸外设
- 通过 `TouchSensor` 类封装
- 需初始化通道和阈值百分比（70%）
- 每次触摸增加电机速度 10%

**任务调度**:
- FreeRTOS 任务，优先级为 5
- 栈大小 4096 字节
- 100ms 采样周期，平衡响应速度和 CPU 占用

---

### 四、电机翅膀控制

#### 功能说明
`InitializeMotorPwm()` 初始化电机 PWM 控制，使用 ESP32 的 LEDC 外设生成 PWM 信号，精确控制电机转速。

#### PWM 初始化

```cpp
void InitializeMotorPwm()
{
    // 配置 LEDC 定时器
    ledc_timer_config_t ledc_timer;
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.duty_resolution = LEDC_TIMER_12_BIT;  // 12位分辨率 (0-4095)
    ledc_timer.timer_num = LEDC_TIMER_0;
    ledc_timer.freq_hz = 5000;  // 5kHz PWM 频率
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer.deconfigure = false;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置 LEDC 通道
    ledc_channel_config_t ledc_channel;
    ledc_channel.gpio_num = MOTOR_GPIO;
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = LEDC_CHANNEL_0;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = LEDC_TIMER_0;
    ledc_channel.duty = 0;  // 初始占空比为 0
    ledc_channel.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    motor_duty_cycle_ = 0;
    ESP_LOGI(TAG, "Motor PWM initialized on GPIO %d, initial duty: 0%%", MOTOR_GPIO);
}
```

#### 占空比控制

**设置占空比**:
```cpp
void SetMotorDutyCycle(uint8_t percentage)
{
    if (percentage > 100)
    {
        percentage = 100;
    }

    // 将百分比转换为 12 位值 (0-4095)
    motor_duty_cycle_ = (percentage * 4095) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, motor_duty_cycle_);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    ESP_LOGI(TAG, "Motor duty cycle set to %d%% (%d)", percentage, motor_duty_cycle_);
}
```

**增加占空比**:
```cpp
void IncreaseMotorDutyCycle(uint8_t increment)
{
    uint8_t current_percentage = (motor_duty_cycle_ * 100) / 4095;
    uint8_t new_percentage = current_percentage + increment;

    if (new_percentage > 100)
    {
        new_percentage = 100;
    }

    SetMotorDutyCycle(new_percentage);
}
```

#### 技术要点

**PWM 参数**:
- 频率: 5kHz，适合电机控制
- 分辨率: 12 位 (0-4095)，提供精细的速度控制
- 占空比范围: 0-100%

**控制逻辑**:
- Touch 1 按下: 设置为 50% 占空比
- Touch 1 释放: 设置为 0% 占空比（停止电机）
- Touch 2 触摸: 每次增加 10% 占空比，上限 100%

**LEDC 外设**:
- 使用低速模式 (LEDC_LOW_SPEED_MODE)
- 定时器 0，通道 0
- 自动时钟源选择

#### 使用示例

```cpp
// 启动电机到半速
SetMotorDutyCycle(50);

// 逐步加速
IncreaseMotorDutyCycle(10);  // 从 50% 增加到 60%
IncreaseMotorDutyCycle(10);  // 从 60% 增加到 70%

// 停止电机
SetMotorDutyCycle(0);
```

---

### 五、电源管理

#### 开机流程 (`PowerOn()`)

1. 调用 `power_manager_.PowerOn()` 更新电源状态
2. 更新 LED 指示灯状态
3. 初始化雷达传感器
4. 初始化电机 PWM
5. 初始化两个触摸传感器
6. 启动传感器监控任务
7. 执行自动唤醒逻辑 (`HandleAutoWake()`)

#### 关机流程 (`PowerOff()`)

1. 调用 `power_manager_.PowerOff()` 更新电源状态
2. 更新 LED 指示灯状态
3. 将设备状态设置为空闲 (`kDeviceStateIdle`)
4. 便于下次开机自动唤醒

#### 自动唤醒 (`HandleAutoWake()`)

当设备处于空闲状态时：
- 如果由 USB 供电，播放成功音效
- 延时 500ms
- 切换聊天状态，激活设备

如果设备尚未进入空闲状态，创建定时器在 500ms 后再次检查。

---

### 六、按键功能

#### 控制按键 (`ctrl_button_`)

- **单击**: 切换聊天状态（打断当前对话）
- **双击**: 如果设备正在启动，进入 WiFi 配置模式
- **长按**: 切换电源开关状态（开机/关机）

#### 启动按键 (`boot_button_`)

使用默认的启动按键功能（未在代码中自定义回调）。

---

### 七、音频系统

#### 音频编解码器

使用 ES8389 音频编解码器，通过 I2C 总线通信：

```cpp
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
```

#### 音频功放控制

- 初始化时将功放引脚设置为低电平（关闭）
- 可通过 `SetAudioAmplifierState()` 函数控制功放开关
- 当前代码中功放控制在开机/关机时被注释掉，可根据需要启用

---

## 编译和烧录

### 环境要求

- ESP-IDF v5.x 或更高版本
- CMake 3.16+
- Python 3.8+

### 编译步骤

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置项目（选择 "雾岸科技 Nano" 板子类型）
idf.py menuconfig

# 编译
idf.py build

# 烧录
idf.py flash

# 监视串口输出
idf.py monitor
```

### 快速构建

```bash
python scripts/release.py fogseek-Nano
```

### 配置文件检查

确保 `config.h` 中所有引脚定义与实际硬件连接一致，特别是：
- ✅ 显示屏 SPI 引脚（MOSI/SCLK/CS/DC/RESET）
- ✅ 雷达传感器 GPIO
- ✅ 电机 PWM GPIO
- ✅ 触摸传感器 GPIO（GPIO44 和 GPIO9）
- ✅ 音频编解码器 I2C 和 I2S 引脚
- ✅ 电源管理相关引脚（HOLD/CHARGING/CHARGE_DONE/ADC）
- ✅ LED 和按键引脚

---

## 应用场景

- 🎮 **智能互动玩具**: 通过触摸和雷达感应实现人机互动
- 🤖 **教育机器人**: 学习传感器融合和 PWM 控制技术
- 🎭 **表情展示设备**: 双屏同步显示丰富表情动画
- 🎵 **语音交互设备**: 支持音效播放和语音对话
- 💡 **创意原型开发**: 模块化设计支持快速迭代和功能扩展

---

## 技术规格

| 项目 | 规格 |
|------|------|
| 主控芯片 | ESP32-S3 |
| 存储 | 16MB Flash + 8MB PSRAM |
| 显示屏 | 2x GC9D01N SPI LCD (240x240) |
| 音频 | ES8389 编解码器，支持双通道麦克风输入 |
| 通信 | WiFi/BLE |
| 传感器 | 雷达 + GPIO44 触摸 + GPIO9 电容触摸 |
| 执行器 | LEDC PWM 电机控制 (5kHz, 12-bit) |
| 接口 | FPC 排线、Type-C、I2C 总线、SPI 总线 |
| 电源 | 锂电池供电，支持充电检测和电源管理 |

---

## 已知问题和改进建议

### 紧急修复（已完成 ✅）

1. **雷达 GPIO 配置错误**: ✅ 已修正为 `GPIO_MODE_INPUT` 并启用下拉电阻
2. **电机关闭逻辑错误**: ✅ 已修正为 `SetMotorDutyCycle(0)`

### 功能增强建议

1. **双屏支持**: 当前代码仅初始化了一个屏幕，需要添加第二个屏幕的初始化代码
2. **雷达中断**: 当前使用轮询方式，可改为 GPIO 中断以提高响应速度
3. **电机电流保护**: 添加电机电流监测，防止过载
4. **触摸防抖**: 添加触摸去抖逻辑，避免误触发
5. **低功耗模式**: 在空闲时降低传感器采样频率或进入深度睡眠

### 代码优化建议

1. 移除未使用的成员变量 (`gpio43_output_enabled_`, `gpio43_current_state_`)
2. 统一日志输出格式
3. 添加更详细的错误处理
4. 考虑将传感器监控任务封装为独立类

---

## 总结

Angel 项目是一个功能丰富的智能玩具平台，集成了显示、传感、音频和执行器等多种模块。通过合理的软件架构设计，实现了模块化、可扩展的系统结构。

### 核心特点
- ✅ 双屏同步显示
- ✅ 多传感器融合（雷达 + 双触摸）
- ✅ PWM 电机控制
- ✅ 智能电源管理
- ✅ 音频交互能力

### 下一步工作
1. 完善双屏显示支持
2. 优化传感器响应性能（改用中断方式）
3. 添加更多交互模式和动画效果
4. 实现低功耗模式
5. 完善测试用例和用户文档

---

**文档版本**: v1.1  
**更新日期**: 2026-04-08  
**维护者**: 雾岸科技开发团队  
**项目名称**: Angel（陈总娃头）  
**硬件平台**: Nano 核心板 + Toy1_V1.1 拓展板
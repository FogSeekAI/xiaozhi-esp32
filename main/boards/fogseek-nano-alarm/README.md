# FogSeek Nano Alarm

基于 FogSeek Nano Toy 硬件平台的 AI 闹钟设备。

## 硬件规格

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3 (16MB Flash + 8MB PSRAM) |
| 显示屏 | 双 160x160 圆形 LCD（GC9D01N），通过 SPI 连接 |
| 音频 | ES8389 编解码器 + NS4150B 功放 |
| LED | 红 ×1 + 绿 ×1 |
| 振动 | 振动电机（TCA6408A P6） |
| 按钮 | BOOT (IO0)、CTRL (IO18) |
| 电源 | 电池供电 + USB 充电 |
| 网络 | WiFi 2.4GHz |

## 闹钟功能

### 闹钟类型

- **绝对时间闹钟**：设置今天/明天的具体时间点（如 "8:30"）
- **相对时间闹钟**：设置 N 秒后的倒计时（如 "5分钟后"）

### 闹钟表现

闹钟触发时会：
1. 🎵 播放预置闹钟音频
2. 📳 振动电机启动
3. 💡 红绿 LED 交替闪烁
4. 😊 双眼显示屏显示闹钟表情
5. 🗣️ AI 语音播报闹钟消息

### 关闭闹钟

按下 **CTRL 按钮** 或 **BOOT 按钮** 即可关闭正在响铃的闹钟。

### AI 对话控制

通过自然语言与 AI 对话来管理闹钟，AI 会调用以下 MCP 工具：

| 工具名 | 功能 | 参数 |
|--------|------|------|
| `self.alarm.set` | 设置闹钟 | type(hour+minute / seconds_from_now), message(可选) |
| `self.alarm.cancel` | 取消闹钟 | id |
| `self.alarm.list` | 列出闹钟 | 无 |
| `self.alarm.get_status` | 闹钟状态 | 无 |

**对话示例：**
- 用户："帮我设置一个8点半的闹钟"
- 用户："5分钟后提醒我关火"
- 用户："取消第一个闹钟"
- 用户："我还有几个闹钟？"

### 持久化

闹钟数据存储在 NVS 中，断电不丢失。设备重启后会自动恢复未触发的闹钟。

## 自定义闹钟音频

将自定义的 OGG 音频文件放入 `main/assets/common/alarm.ogg`，然后在 `fogseek-nano-alarm.cc` 中将 `Lang::Sounds::OGG_EXCLAMATION` 替换为对应的音频引用。

## 按键说明

| 按键 | 单击 | 双击 | 长按 |
|------|------|------|------|
| CTRL | 关闭闹钟 / 静音切换 | 进入配网模式 | 开关机 |
| BOOT | 关闭闹钟 | - | - |

## 编译

```bash
cd xiaozhi-esp32-private
# 方式一：自动构建
python scripts/release.py fogseek-nano-alarm

# 方式二：手动构建
idf.py set-target esp32s3
idf.py menuconfig  # 选择 Board Type -> Nano Alarm
idf.py build
```

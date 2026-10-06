# 面包板 S3CAM + PCA9685 四足机器狗

把立创开源课程项目 [EDA-RobotPro AI智能机器狗](https://oshwhub.com/course-examples/eda-robotpro)
的**软件部分**移植到本仓库现有的 `bread-compact-wifi-s3cam` 硬件上。

不重画 PCB、不焊新板子：复用已有的面包板 ESP32-S3 CAM 开发板 + PCA9685 舵机板，
只补 4 个舵机、一路 5V 大电流电源和 3D 打印结构件。

原项目上游代码已经在仓库里：`main/boards/lceda-course-examples/eda-robot-pro/`。

## 与原版 EDA-Robot Pro 的差异（改动清单）

| # | 改了什么 | 原版 Pro | 本板 | 原因 |
|---|---------|---------|------|------|
| 1 | 舵机输出层 | `oscillator.cc` 直调 LEDC，一个舵机占一个 GPIO | 改成写 PCA9685 通道 | s3cam 板 GPIO 已被摄像头/LCD/音频/PSRAM 占满，没有 4 个空闲脚直出 50Hz PWM |
| 2 | 舵机参数载体 | `Attach(gpio)` | `Attach(pca9685_channel)` | 同上；步态/校准/脉宽映射全部沿用原版，行为一致 |
| 3 | 引脚宏 | `LEFT_FRONT_LEG_PIN` 等 4 个 GPIO | `LEFT_FRONT_LEG_CH` 等 4 个通道号 | 语义变化 |
| 4 | 显示屏 | SSD1306 128×64 OLED（I2C） | ST7789 240×240 LCD（SPI） | 用板载屏，沿用 s3cam 的 `SpiLcdDisplay` |
| 5 | 摄像头 | 无 | 不初始化（OV2640 仍插在板上） | 狗不需要，且避免摄像头 IO 抢线；以后想加视觉再打开 |
| 6 | 履带电机工具 | 无 | 不注册 `self.car.drive` | 机器狗和履带挖掘机是两套固件，烧哪个用哪个 |
| 7 | 板级 BSP | `eda_robot_pro.cc` | `s3cam_dog_board.cc` | 音频/按键/背光沿用 s3cam 实现 |

**没改的部分**（整段照搬）：`eda_dog_movements.cc`(576 行步态)、`eda_dog_controller.cc`
(动作队列 + 11 个 `self.dog.*` MCP 工具 + NVS 舵机微调)，逻辑与原版逐字一致。

## 接线

| PCA9685 | 接到 | 说明 |
|---------|------|------|
| VCC | ESP32 板 3.3V | I2C 逻辑电平跟随 VCC；接 5V 会让 3.3V 的 SDA/SCL 处于 VIH 门槛边缘 |
| GND | ESP32 板 GND | 必须共地 |
| V+ | 舵机独立 5V 电源 | **3A 以上**，4 个 MG90S 同时堵转可达 2.5A+ |
| SDA | GPIO14 | 与履带挖掘机同一根总线 |
| SCL | GPIO3 | 同上 |
| ch0 | 左前腿舵机 | |
| ch1 | 左后腿舵机 | |
| ch2 | 右前腿舵机 | |
| ch3 | 右后腿舵机 | |

- 舵机信号线用 PCA9685 的 PWM 排针，**不要**从 ESP32 的 5V/3V3 脚给舵机取电。
- 与履带挖掘机共用一根 I2C 总线时，把本板的 PCA9685 地址改成 0x41
  （`config.h` 里的 `DOG_SERVO_PCA9685_ADDR`）。PCA9685 的预分频是整块芯片共享的，
  50Hz 舵机和 1526Hz 电机不能挂同一块板。
- 舵机一定买 **180° 版**（内置限位、免校准）。

## 编译烧录

```bash
source <esp-idf>/export.sh
python scripts/build.py bread-compact-wifi-s3cam-dog --no-version-check
idf.py -p <COM口> flash monitor
```

或者在 menuconfig 里手动选：
`Xiaozhi Assistant -> Board Type -> Bread Compact Wi-Fi + LCD + PCA9685 Robot Dog`

## 语音指令（MCP 工具）

唤醒小智后直接说口语化指令，由 LLM 调工具：

- 「向前走三步」「后退」「左转」「右转」
- 「坐下」「站起来」「伸个懒腰」「摇一摇」
- 「抬左前腿」「抬右后腿」
- 「停下」
- 「把左前腿微调调成 5 度」（永久保存到 NVS）

## 舵机校准

180° 舵机自带限位器、无需校准转速。只需为了让四条腿站得平：
先用 `self.dog.get_trims` 看当前值，再逐个 `self.dog.set_trim` 调姿态，
每次调完会自动回 Home 便于观察。

## 结构件（待办）

原版 3D 外壳是按 Pro 版自绘 PCB 的尺寸和孔位建模的，本板尺寸不同，
**不能直接用**。两条路：

1. 取嘉立创云 CAD 的 Pro 外壳工程，改板槽/螺柱尺寸后重新打印（推荐光固化）
2. 外挂式：舵机按 Pro 的布局装底壳，板子绑在狗背上方

## 限制

- 舵机板没接时固件照常启动（打日志警告），语音对话可用，只有动作无效
- 没有加陀螺仪/避障，走路是开环步态，地面太滑会走偏

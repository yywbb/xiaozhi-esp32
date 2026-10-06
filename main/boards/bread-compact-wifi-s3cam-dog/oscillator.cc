//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- (c) txp666 for esp32, 202503
//-- GPL license
//--------------------------------------------------------------
//-- 移植说明：原版用 ESP32 LEDC（每个舵机一个 GPIO + 一个 LEDC 通道，
//-- 定时器固定 50Hz、13 位占空比）。本板没有空闲 GPIO，改用 PCA9685：
//--   Attach(channel)  -> 记录通道，不再配置 LEDC
//--   Write(position)  -> 角度换算成 PCA9685 的 12 位翻转点
//-- 脉宽映射完全沿用原版（0~180 度 -> 0.5~2.5ms），所以步态参数、
//-- 舵机微调值、MCP 工具接口都与 EDA-Robot Pro 原版一致。
//--------------------------------------------------------------
#include "oscillator.h"

#include <esp_timer.h>

#include <algorithm>
#include <cmath>

static const char* TAG = "Oscillator";

extern unsigned long IRAM_ATTR millis();

Oscillator::Oscillator(int trim) {
    trim_ = trim;
    diff_limit_ = 0;
    is_attached_ = false;

    sampling_period_ = 30;
    period_ = 2000;
    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;

    amplitude_ = 45;
    phase_ = 0;
    phase0_ = 0;
    offset_ = 0;
    stop_ = false;
    rev_ = false;

    pos_ = 90;
    channel_ = -1;
    previous_millis_ = 0;
}

Oscillator::~Oscillator() {
    Detach();
}

uint32_t Oscillator::AngleToTicks(int angle) {
    // 与原版 Write() 的占空比公式保持一致：0~180 度 -> 0.5~2.5ms 脉宽，
    // 换算成一个 20ms 周期内的 12 位翻转点（4096 计数）
    const float pulse_us =
        angle / 180.0f * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) +
        SERVO_MIN_PULSEWIDTH_US;
    return (uint32_t)(pulse_us * 4096.0f / 20000.0f);
}

bool Oscillator::NextSample() {
    current_millis_ = millis();

    if (current_millis_ - previous_millis_ > sampling_period_) {
        previous_millis_ = current_millis_;
        return true;
    }

    return false;
}

void Oscillator::Attach(int channel, bool rev) {
    if (is_attached_) {
        Detach();
    }

    channel_ = channel;
    rev_ = rev;

    auto& bus = DogServoBus::GetInstance();
    if (!bus.IsPresent()) {
        // 舵机板没接：不挂载，动作函数会因为 is_attached_ 为假而静默跳过，
        // 板子照常启动、联网、语音对话可用。
        ESP_LOGW(TAG, "PCA9685 不在线，通道 %d 未挂载", channel);
        is_attached_ = false;
        return;
    }

    bus.SetChannelOff((uint8_t)channel_, AngleToTicks(pos_));

    previous_servo_command_millis_ = millis();
    is_attached_ = true;
}

void Oscillator::Detach() {
    if (!is_attached_)
        return;

    is_attached_ = false;
}

void Oscillator::SetT(unsigned int T) {
    period_ = T;

    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;
}

void Oscillator::SetPosition(int position) {
    Write(position);
}

void Oscillator::Refresh() {
    if (NextSample()) {
        if (!stop_) {
            int pos = std::round(amplitude_ * std::sin(phase_ + phase0_) + offset_);
            if (rev_)
                pos = -pos;
            Write(pos + 90);
        }

        phase_ = phase_ + inc_;
    }
}

void Oscillator::Write(int position) {
    if (!is_attached_)
        return;

    long currentMillis = millis();
    if (diff_limit_ > 0) {
        int limit = std::max(
            1, (((int)(currentMillis - previous_servo_command_millis_)) * diff_limit_) / 1000);
        if (abs(position - pos_) > limit) {
            pos_ += position < pos_ ? -limit : limit;
        } else {
            pos_ = position;
        }
    } else {
        pos_ = position;
    }
    previous_servo_command_millis_ = currentMillis;

    int angle = pos_ + trim_;

    angle = std::min(std::max(angle, 0), 180);

    DogServoBus::GetInstance().SetChannelOff((uint8_t)channel_, AngleToTicks(angle));
}

//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- (c) txp666 for esp32, 202503
//-- GPL license
//--------------------------------------------------------------
//-- 本文件由 lceda-course-examples/eda-robot-pro/oscillator.h 移植而来，
//-- 唯一改动：输出层从 ESP32 的 LEDC 改成 PCA9685 的通道。
//-- 原版 Attach(pin) 里的 pin 在这里是 PCA9685 的通道号（0~15）。
//--------------------------------------------------------------
#ifndef __OSCILLATOR_H__
#define __OSCILLATOR_H__

#include "pca9685_servo.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define M_PI 3.14159265358979323846

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI) / 180
#endif

#define SERVO_MIN_PULSEWIDTH_US 500          // 最小脉宽（微秒）
#define SERVO_MAX_PULSEWIDTH_US 2500         // 最大脉宽（微秒）
#define SERVO_MIN_DEGREE -90                 // 最小角度
#define SERVO_MAX_DEGREE 90                  // 最大角度

class Oscillator {
public:
  Oscillator(int trim = 0);
  ~Oscillator();
  void Attach(int channel, bool rev = false);  // channel: PCA9685 通道号
  void Detach();

  void SetA(unsigned int amplitude) { amplitude_ = amplitude; };
  void SetO(unsigned int offset) { offset_ = offset; };
  void SetPh(double Ph) { phase0_ = Ph; };
  void SetT(unsigned int period);
  void SetTrim(int trim) { trim_ = trim; };
  void SetLimiter(int diff_limit) { diff_limit_ = diff_limit; };
  void DisableLimiter() { diff_limit_ = 0; };
  int GetTrim() { return trim_; };
  void SetPosition(int position);
  void Stop() { stop_ = true; };
  void Play() { stop_ = false; };
  void Reset() { phase_ = 0; };
  void Refresh();
  int GetPosition() { return pos_; }

private:
  bool NextSample();
  void Write(int position);
  uint32_t AngleToTicks(int angle);   // 0~180 度 -> PCA9685 翻转计数值

private:
  bool is_attached_;

  //-- Oscillators parameters
  unsigned int amplitude_; //-- Amplitude (degrees)
  int offset_;             //-- Offset (degrees)
  unsigned int period_;    //-- Period (miliseconds)
  double phase0_;          //-- Phase (radians)

  //-- Internal variables
  int pos_;                      //-- Current servo pos
  int channel_;                  //-- PCA9685 channel where the servo is connected
  int trim_;                     //-- Calibration offset
  double phase_;                 //-- Current phase
  double inc_;                   //-- Increment of phase
  double number_samples_;        //-- Number of samples
  unsigned int sampling_period_; //-- sampling period (ms)

  long previous_millis_;
  long current_millis_;

  //-- Oscillation mode. If true, the servo is stopped
  bool stop_;

  //-- Reverse mode
  bool rev_;

  int diff_limit_;
  long previous_servo_command_millis_;
};

#endif // __OSCILLATOR_H__

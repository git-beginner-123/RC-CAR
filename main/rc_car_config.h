#pragma once

#include "esp_camera.h"

// Wi-Fi AP exposed by the car.
#define RC_CAR_AP_SSID           "RC_CAR"

// Assumed common ESP32-S3-CAM + OV2640 DVP mapping.
// If your module differs, adjust the pins below.
#define RC_CAR_CAM_SCCB_SCL_IO   5
#define RC_CAR_CAM_SCCB_SDA_IO   4
#define RC_CAR_CAM_XCLK_IO       15
#define RC_CAR_CAM_PCLK_IO       13
#define RC_CAR_CAM_VSYNC_IO      6
#define RC_CAR_CAM_HREF_IO       7
#define RC_CAR_CAM_D0_IO         11
#define RC_CAR_CAM_D1_IO         9
#define RC_CAR_CAM_D2_IO         8
#define RC_CAR_CAM_D3_IO         10
#define RC_CAR_CAM_D4_IO         12
#define RC_CAR_CAM_D5_IO         18
#define RC_CAR_CAM_D6_IO         17
#define RC_CAR_CAM_D7_IO         16
#define RC_CAR_CAM_PWDN_IO       (-1)
// OV_RESET is tied to EN on this board, not a dedicated GPIO.
#define RC_CAR_CAM_RESET_IO      (-1)
#define RC_CAR_CAM_XCLK_FREQ_HZ  20000000
#define RC_CAR_CAM_FRAME_SIZE    FRAMESIZE_QVGA
#define RC_CAR_CAM_JPEG_QUALITY  18
#define RC_CAR_CAM_FB_COUNT      2

// Motor defaults are intentionally disabled until the real H-bridge pins are known.
// Fill these values with your board wiring before driving hardware.
#define RC_CAR_MOTOR_LEFT_IN1    (-1)
#define RC_CAR_MOTOR_LEFT_IN2    (-1)
#define RC_CAR_MOTOR_LEFT_PWM    (-1)
#define RC_CAR_MOTOR_RIGHT_IN1   (-1)
#define RC_CAR_MOTOR_RIGHT_IN2   (-1)
#define RC_CAR_MOTOR_RIGHT_PWM   (-1)

#define RC_CAR_MOTOR_PWM_FREQ_HZ 20000
#define RC_CAR_MOTOR_SPEED_PCT   80

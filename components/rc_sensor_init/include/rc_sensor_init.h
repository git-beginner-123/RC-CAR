#pragma once

#include "driver/i2c_master.h"
#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_cam_sensor_device_t* cam;
    esp_sccb_io_handle_t sccb_handle;
    i2c_master_bus_handle_t i2c_bus_handle;
} RcSensorHandle;

typedef struct {
    int i2c_port_num;
    int i2c_sda_io_num;
    int i2c_scl_io_num;
    esp_cam_sensor_port_t port;
    const char* format_name;
    int pwdn_pin;
    int reset_pin;
    int xclk_pin;
    int xclk_freq_hz;
} RcSensorConfig;

esp_err_t RcSensor_Init(const RcSensorConfig* sensor_config, RcSensorHandle* out_sensor_handle);
void RcSensor_Deinit(RcSensorHandle* sensor_handle);

#ifdef __cplusplus
}
#endif

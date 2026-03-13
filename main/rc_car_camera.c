#include "rc_car_camera.h"

#include <stdio.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "rc_car_config.h"

static const char *TAG = "rc_car_camera";

typedef struct {
    bool ready;
    char last_error[96];
} rc_car_camera_state_t;

static rc_car_camera_state_t s_cam = {0};

bool RcCarCamera_Init(void)
{
    camera_config_t cfg = {
        .pin_pwdn = RC_CAR_CAM_PWDN_IO,
        .pin_reset = RC_CAR_CAM_RESET_IO,
        .pin_xclk = RC_CAR_CAM_XCLK_IO,
        .pin_sccb_sda = RC_CAR_CAM_SCCB_SDA_IO,
        .pin_sccb_scl = RC_CAR_CAM_SCCB_SCL_IO,
        .pin_d7 = RC_CAR_CAM_D7_IO,
        .pin_d6 = RC_CAR_CAM_D6_IO,
        .pin_d5 = RC_CAR_CAM_D5_IO,
        .pin_d4 = RC_CAR_CAM_D4_IO,
        .pin_d3 = RC_CAR_CAM_D3_IO,
        .pin_d2 = RC_CAR_CAM_D2_IO,
        .pin_d1 = RC_CAR_CAM_D1_IO,
        .pin_d0 = RC_CAR_CAM_D0_IO,
        .pin_vsync = RC_CAR_CAM_VSYNC_IO,
        .pin_href = RC_CAR_CAM_HREF_IO,
        .pin_pclk = RC_CAR_CAM_PCLK_IO,
        .xclk_freq_hz = RC_CAR_CAM_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = RC_CAR_CAM_FRAME_SIZE,
        .jpeg_quality = RC_CAR_CAM_JPEG_QUALITY,
        .fb_count = RC_CAR_CAM_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = 0,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        s_cam.ready = false;
        snprintf(s_cam.last_error, sizeof(s_cam.last_error), "esp_camera_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_cam.last_error);
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_quality(sensor, RC_CAR_CAM_JPEG_QUALITY);
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 1);
        sensor->set_saturation(sensor, 0);
    }

    s_cam.ready = true;
    s_cam.last_error[0] = '\0';
    ESP_LOGI(TAG, "camera ready: jpeg frame_size=%d quality=%d fb_count=%d",
             RC_CAR_CAM_FRAME_SIZE, RC_CAR_CAM_JPEG_QUALITY, RC_CAR_CAM_FB_COUNT);
    return true;
}

void RcCarCamera_Deinit(void)
{
    (void)esp_camera_deinit();
    s_cam.ready = false;
}

bool RcCarCamera_IsReady(void)
{
    return s_cam.ready;
}

const char *RcCarCamera_LastError(void)
{
    return s_cam.last_error;
}

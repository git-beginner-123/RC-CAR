#include "rc_sensor_init.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_cam_sensor_detect.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"

static const char* TAG = "rc_sensor_init";

#define RC_CAM_SCCB_FREQ_HZ (10 * 1000)

static void log_sccb_probe(i2c_master_bus_handle_t i2c_bus_handle, uint16_t addr)
{
    esp_sccb_io_handle_t sccb_handle = NULL;
    sccb_i2c_config_t i2c_config = {
        .scl_speed_hz = RC_CAM_SCCB_FREQ_HZ,
        .device_address = addr,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    };
    esp_err_t err = sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &sccb_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sccb create failed addr=0x%02x: %s", addr, esp_err_to_name(err));
        return;
    }

    uint8_t pid = 0;
    uint8_t ver = 0;
    esp_err_t pid_err = esp_sccb_transmit_receive_reg_a8v8(sccb_handle, 0x0A, &pid);
    esp_err_t ver_err = esp_sccb_transmit_receive_reg_a8v8(sccb_handle, 0x0B, &ver);
    if (pid_err == ESP_OK || ver_err == ESP_OK) {
        ESP_LOGI(TAG, "sccb probe addr=0x%02x ack pid=0x%02x(%s) ver=0x%02x(%s)",
                 addr,
                 pid, esp_err_to_name(pid_err),
                 ver, esp_err_to_name(ver_err));
    } else {
        ESP_LOGI(TAG, "sccb probe addr=0x%02x no reg response pid=%s ver=%s",
                 addr, esp_err_to_name(pid_err), esp_err_to_name(ver_err));
    }
    esp_sccb_del_i2c_io(sccb_handle);
}

esp_err_t RcSensor_Init(const RcSensorConfig* sensor_config, RcSensorHandle* out_sensor_handle)
{
    if (!sensor_config || !out_sensor_handle || !sensor_config->format_name) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = sensor_config->i2c_sda_io_num,
        .scl_io_num = sensor_config->i2c_scl_io_num,
        .i2c_port = sensor_config->i2c_port_num,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle), TAG, "create i2c bus failed");

    log_sccb_probe(i2c_bus_handle, 0x30);
    log_sccb_probe(i2c_bus_handle, 0x3c);
    log_sccb_probe(i2c_bus_handle, 0x21);
    log_sccb_probe(i2c_bus_handle, 0x42);

    esp_cam_sensor_config_t cam_config = {
        .reset_pin = sensor_config->reset_pin,
        .pwdn_pin = sensor_config->pwdn_pin,
        .xclk_pin = sensor_config->xclk_pin,
        .xclk_freq_hz = sensor_config->xclk_freq_hz,
    };

    esp_cam_sensor_device_t* cam = NULL;
    for (esp_cam_sensor_detect_fn_t* p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = RC_CAM_SCCB_FREQ_HZ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ESP_LOGI(TAG, "probe sensor addr=0x%02x port=%d xclk=%d", p->sccb_addr, (int)p->port, sensor_config->xclk_freq_hz);
        ESP_RETURN_ON_ERROR(sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &cam_config.sccb_handle), TAG, "create sccb failed");

        cam_config.sensor_port = p->port;
        cam = (*(p->detect))(&cam_config);
        if (cam) {
            ESP_LOGI(TAG, "detected sensor name=%s addr=0x%02x port=%d", cam->name ? cam->name : "unknown", p->sccb_addr, (int)p->port);
            if (p->port != sensor_config->port) {
                ESP_LOGE(TAG, "camera interface mismatch");
                esp_sccb_del_i2c_io(cam_config.sccb_handle);
                i2c_del_master_bus(i2c_bus_handle);
                return ESP_ERR_INVALID_RESPONSE;
            }
            break;
        }
        esp_sccb_del_i2c_io(cam_config.sccb_handle);
        cam_config.sccb_handle = NULL;
    }

    if (!cam) {
        i2c_del_master_bus(i2c_bus_handle);
        ESP_LOGE(TAG, "failed to detect camera sensor");
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_sensor_format_array_t cam_fmt_array = {0};
    esp_cam_sensor_query_format(cam, &cam_fmt_array);
    const esp_cam_sensor_format_t* found_fmt = NULL;
    for (int i = 0; i < cam_fmt_array.count; i++) {
        ESP_LOGI(TAG, "sensor format[%d]=%s", i, cam_fmt_array.format_array[i].name);
        if (strcmp(cam_fmt_array.format_array[i].name, sensor_config->format_name) == 0) {
            found_fmt = &cam_fmt_array.format_array[i];
        }
    }
    if (!found_fmt) {
        ESP_LOGE(TAG, "unsupported format for sensor=%s: %s", cam->name ? cam->name : "unknown", sensor_config->format_name);
        esp_sccb_del_i2c_io(cam_config.sccb_handle);
        i2c_del_master_bus(i2c_bus_handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(cam, found_fmt), TAG, "set format failed");

    int enable_flag = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag), TAG, "start sensor stream failed");

    out_sensor_handle->cam = cam;
    out_sensor_handle->i2c_bus_handle = i2c_bus_handle;
    out_sensor_handle->sccb_handle = cam_config.sccb_handle;
    return ESP_OK;
}

void RcSensor_Deinit(RcSensorHandle* sensor_handle)
{
    if (!sensor_handle) return;
    if (sensor_handle->cam) {
        esp_cam_sensor_del_dev(sensor_handle->cam);
        sensor_handle->cam = NULL;
    }
    if (sensor_handle->sccb_handle) {
        esp_sccb_del_i2c_io(sensor_handle->sccb_handle);
        sensor_handle->sccb_handle = NULL;
    }
    if (sensor_handle->i2c_bus_handle) {
        i2c_del_master_bus(sensor_handle->i2c_bus_handle);
        sensor_handle->i2c_bus_handle = NULL;
    }
}

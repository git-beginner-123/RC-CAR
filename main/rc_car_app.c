#include "rc_car_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm_wifi.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "rc_car_camera.h"
#include "rc_car_config.h"
#include "rc_car_motor.h"

static const char *TAG = "rc_car_app";
#define PART_BOUNDARY "123456789000000000000987654321"
static const int64_t kStreamSlowFrameWarnMs = 800;
static const uint16_t kCtrlUdpPort = 3333;
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

typedef struct {
    httpd_handle_t camera_httpd;
    httpd_handle_t stream_httpd;
    TaskHandle_t ctrl_udp_task;
    RcCarMotorCmd last_cmd;
    uint32_t cmd_count;
    int64_t last_cmd_ms;
} rc_car_state_t;

static rc_car_state_t s_state = {
    .camera_httpd = NULL,
    .stream_httpd = NULL,
    .ctrl_udp_task = NULL,
    .last_cmd = kRcCarMotorCmdStop,
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *chunk = (jpg_chunking_t *)arg;
    if (!index) {
        chunk->len = 0;
    }
    if (httpd_resp_send_chunk(chunk->req, (const char *)data, len) != ESP_OK) {
        return 0;
    }
    chunk->len += len;
    return len;
}

static esp_err_t set_frame_timestamp_header(httpd_req_t *req, const camera_fb_t *fb)
{
    char ts[32];
    snprintf(ts, sizeof(ts), "%" PRIu32 ".%06" PRIu32,
             (uint32_t)fb->timestamp.tv_sec, (uint32_t)fb->timestamp.tv_usec);
    return httpd_resp_set_hdr(req, "X-Timestamp", ts);
}

static bool parse_cmd(const char *text, RcCarMotorCmd *out_cmd)
{
    if (!text || !out_cmd) {
        return false;
    }

    if (strcmp(text, "forward") == 0 || strcmp(text, "up") == 0) {
        *out_cmd = kRcCarMotorCmdForward;
        return true;
    }
    if (strcmp(text, "backward") == 0 || strcmp(text, "down") == 0) {
        *out_cmd = kRcCarMotorCmdBackward;
        return true;
    }
    if (strcmp(text, "left") == 0) {
        *out_cmd = kRcCarMotorCmdLeft;
        return true;
    }
    if (strcmp(text, "right") == 0) {
        *out_cmd = kRcCarMotorCmdRight;
        return true;
    }
    if (strcmp(text, "stop") == 0 || strcmp(text, "enter") == 0) {
        *out_cmd = kRcCarMotorCmdStop;
        return true;
    }
    return false;
}

static void apply_command(RcCarMotorCmd cmd)
{
    s_state.last_cmd = cmd;
    s_state.cmd_count++;
    s_state.last_cmd_ms = now_ms();
    RcCarMotor_Apply(cmd);
}

static void ctrl_udp_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "udp ctrl socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(kCtrlUdpPort),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "udp ctrl bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 500000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "udp ctrl listening on port %u", (unsigned)kCtrlUdpPort);

    while (true) {
        char buf[32];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from_addr, &from_len);
        if (len < 0) {
            continue;
        }
        buf[len] = '\0';

        RcCarMotorCmd cmd = kRcCarMotorCmdStop;
        if (!parse_cmd(buf, &cmd)) {
            ESP_LOGW(TAG, "udp ctrl invalid cmd: %s", buf);
            continue;
        }

        ESP_LOGI(TAG, "udp ctrl cmd=%s", buf);
        apply_command(cmd);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET /");

    static const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RC_CAR Camera</title>"
        "<style>"
        "html,body{margin:0;height:100%;background:#000;color:#fff;font-family:sans-serif}"
        "body{display:flex;align-items:center;justify-content:center}"
        ".wrap{width:min(92vw,480px);text-align:center}"
        "a{display:block;margin:16px 0;padding:18px 20px;border:1px solid #666;border-radius:12px;color:#fff;text-decoration:none;background:#111}"
        "p{color:#bbb}"
        "</style></head><body><div class='wrap'>"
        "<h1>RC_CAR Camera</h1>"
        "<a href='/snapshot_view'>Open Snapshot View</a>"
        "<a href='/capture_view'>Open Capture</a>"
        "<a href='/stream_view'>Open Stream (Experimental)</a>"
        "<p>Use stream and capture separately to avoid overlapping browser requests.</p>"
        "</div></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t empty_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t stream_compat_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET /stream (compat)");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/snapshot_view");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t stream_view_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET /stream_view");

    static const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RC_CAR Stream</title>"
        "<style>"
        "html,body{margin:0;background:#000;color:#fff;font-family:sans-serif}"
        "body{padding:12px}"
        "img{display:block;width:100%;height:auto;background:#000}"
        "button{margin:16px 0;padding:14px 18px;border:1px solid #666;border-radius:12px;background:#111;color:#fff}"
        "a{color:#9cf}"
        "</style></head><body>"
        "<p><a href='/'>Back</a></p>"
        "<button id='reload'>Reload Stream</button>"
        "<img id='stream' alt='camera stream'>"
        "<script>"
        "function openStream(){"
        "document.getElementById('stream').src='http://'+location.hostname+':81/stream?t='+Date.now();"
        "}"
        "document.getElementById('reload').addEventListener('click',openStream);"
        "openStream();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_view_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET /capture_view");

    static const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RC_CAR Capture</title>"
        "<style>"
        "html,body{margin:0;background:#000;color:#fff;font-family:sans-serif}"
        "body{padding:12px;text-align:center}"
        "img{display:block;width:100%;height:auto;background:#000}"
        "button{margin:16px 0;padding:14px 18px;border:1px solid #666;border-radius:12px;background:#111;color:#fff}"
        "a{color:#9cf}"
        "</style></head><body>"
        "<p><a href='/'>Back</a></p>"
        "<button id='capture'>Capture</button>"
        "<img id='frame' alt='capture'>"
        "<script>"
        "document.getElementById('capture').addEventListener('click',function(){"
        "document.getElementById('frame').src='/capture?t='+Date.now();"
        "});"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snapshot_view_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET /snapshot_view");

    static const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RC_CAR Snapshot View</title>"
        "<style>"
        "html,body{margin:0;background:#000;color:#fff;font-family:sans-serif}"
        "body{padding:12px;text-align:center}"
        "img{display:block;width:100%;height:auto;background:#000}"
        "button,select{margin:16px 8px;padding:14px 18px;border:1px solid #666;border-radius:12px;background:#111;color:#fff}"
        "a{color:#9cf}"
        "</style></head><body>"
        "<p><a href='/'>Back</a></p>"
        "<button id='toggle'>Pause</button>"
        "<button id='step'>Step</button>"
        "<select id='interval'>"
        "<option value='120'>120 ms</option>"
        "<option value='200' selected>200 ms</option>"
        "<option value='300'>300 ms</option>"
        "<option value='500'>500 ms</option>"
        "</select>"
        "<img id='frame' alt='snapshot stream'>"
        "<script>"
        "var running=true;"
        "var inflight=false;"
        "var frame=document.getElementById('frame');"
        "var interval=document.getElementById('interval');"
        "var inflightSince=0;"
        "var lastRequestAt=0;"
        "function requestFrame(){"
        "if(!running||inflight) return;"
        "inflight=true;"
        "inflightSince=Date.now();"
        "lastRequestAt=inflightSince;"
        "frame.src='/capture?t='+inflightSince;"
        "}"
        "setInterval(function(){"
        "if(!running) return;"
        "if(inflight && (Date.now()-inflightSince)>1500){"
        "inflight=false;"
        "}"
        "if(!inflight && (Date.now()-lastRequestAt)>=parseInt(interval.value,10)) requestFrame();"
        "}, 100);"
        "document.getElementById('toggle').addEventListener('click',function(){"
        "running=!running;"
        "this.textContent=running?'Pause':'Resume';"
        "if(running && !inflight) requestFrame();"
        "});"
        "document.getElementById('step').addEventListener('click',function(){"
        "if(inflight) return;"
        "requestFrame();"
        "});"
        "interval.addEventListener('change',function(){"
        "if(running&&!inflight) requestFrame();"
        "});"
        "frame.addEventListener('load',function(){"
        "inflight=false;"
        "});"
        "frame.addEventListener('error',function(){"
        "inflight=false;"
        "});"
        "setTimeout(requestFrame, parseInt(interval.value,10));"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_get_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    }
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Connection", "close");
    }
    if (res == ESP_OK) {
        res = set_frame_timestamp_header(req, fb);
    }

    if (res == ESP_OK) {
        if (fb->format == PIXFORMAT_JPEG) {
            res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        } else {
            jpg_chunking_t chunk = {
                .req = req,
                .len = 0,
            };
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &chunk) ? ESP_OK : ESP_FAIL;
            if (res == ESP_OK) {
                res = httpd_resp_send_chunk(req, NULL, 0);
            }
        }
    }

    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t ping_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "OK\n");
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    struct timeval timestamp = {0};
    esp_err_t res = ESP_OK;
    size_t jpg_buf_len = 0;
    uint8_t *jpg_buf = NULL;
    char part_buf[128];
    int64_t last_frame_us = 0;

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "stream set type failed: %s", esp_err_to_name(res));
        return res;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "camera capture failed");
            res = ESP_FAIL;
        } else {
            timestamp.tv_sec = fb->timestamp.tv_sec;
            timestamp.tv_usec = fb->timestamp.tv_usec;
            if (fb->format != PIXFORMAT_JPEG) {
                bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if (!jpeg_converted) {
                    ESP_LOGE(TAG, "jpeg conversion failed");
                    res = ESP_FAIL;
                }
            } else {
                jpg_buf = fb->buf;
                jpg_buf_len = fb->len;
            }
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        }
        if (res == ESP_OK) {
            int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                                (unsigned)jpg_buf_len, (int)timestamp.tv_sec, (int)timestamp.tv_usec);
            if (hlen < 0 || hlen >= (int)sizeof(part_buf)) {
                res = ESP_FAIL;
            } else {
                res = httpd_resp_send_chunk(req, part_buf, (size_t)hlen);
            }
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            jpg_buf = NULL;
        } else if (jpg_buf) {
            free(jpg_buf);
            jpg_buf = NULL;
        }

        if (res != ESP_OK) {
            ESP_LOGW(TAG, "stream send failed: %s", esp_err_to_name(res));
            break;
        }

        if (!last_frame_us) {
            last_frame_us = esp_timer_get_time();
        } else {
            int64_t now_us = esp_timer_get_time();
            int64_t frame_time_ms = (now_us - last_frame_us) / 1000;
            last_frame_us = now_us;
            if (frame_time_ms >= kStreamSlowFrameWarnMs) {
                ESP_LOGW(TAG, "slow stream frame: %u bytes, %lld ms, %.1f fps",
                         (unsigned)jpg_buf_len, frame_time_ms, 1000.0f / (float)frame_time_ms);
            }
        }
    }

    return res;
}

static esp_err_t drive_get_handler(httpd_req_t *req)
{
    char query[64];
    char cmd_text[24];
    RcCarMotorCmd cmd = kRcCarMotorCmdStop;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "cmd", cmd_text, sizeof(cmd_text)) != ESP_OK ||
        !parse_cmd(cmd_text, &cmd)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or invalid cmd");
    }

    ESP_LOGI(TAG, "http GET /api/drive?cmd=%s", cmd_text);
    apply_command(cmd);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK\n");
}

static bool start_http_server(void)
{
    if (s_state.camera_httpd || s_state.stream_httpd) {
        return true;
    }

    httpd_config_t camera_cfg = HTTPD_DEFAULT_CONFIG();
    camera_cfg.max_uri_handlers = 12;
    camera_cfg.stack_size = 8192;
    camera_cfg.max_open_sockets = 5;
    camera_cfg.send_wait_timeout = 30;
    camera_cfg.recv_wait_timeout = 30;
    camera_cfg.keep_alive_enable = true;
    camera_cfg.lru_purge_enable = true;

    if (httpd_start(&s_state.camera_httpd, &camera_cfg) != ESP_OK) {
        s_state.camera_httpd = NULL;
        return false;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t capture = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_get_handler,
    };
    httpd_uri_t ping = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = ping_get_handler,
    };
    httpd_uri_t stream_view = {
        .uri = "/stream_view",
        .method = HTTP_GET,
        .handler = stream_view_get_handler,
    };
    httpd_uri_t capture_view = {
        .uri = "/capture_view",
        .method = HTTP_GET,
        .handler = capture_view_get_handler,
    };
    httpd_uri_t snapshot_view = {
        .uri = "/snapshot_view",
        .method = HTTP_GET,
        .handler = snapshot_view_get_handler,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = empty_204_handler,
    };
    httpd_uri_t apple_touch_icon = {
        .uri = "/apple-touch-icon-120x120-precomposed.png",
        .method = HTTP_GET,
        .handler = empty_204_handler,
    };
    httpd_uri_t apple_touch_icon_plain = {
        .uri = "/apple-touch-icon-120x120.png",
        .method = HTTP_GET,
        .handler = empty_204_handler,
    };
    httpd_uri_t stream_compat = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_compat_handler,
    };
    httpd_uri_t drive = {
        .uri = "/api/drive",
        .method = HTTP_GET,
        .handler = drive_get_handler,
    };

    httpd_register_uri_handler(s_state.camera_httpd, &root);
    httpd_register_uri_handler(s_state.camera_httpd, &ping);
    httpd_register_uri_handler(s_state.camera_httpd, &stream_view);
    httpd_register_uri_handler(s_state.camera_httpd, &capture_view);
    httpd_register_uri_handler(s_state.camera_httpd, &snapshot_view);
    httpd_register_uri_handler(s_state.camera_httpd, &capture);
    httpd_register_uri_handler(s_state.camera_httpd, &favicon);
    httpd_register_uri_handler(s_state.camera_httpd, &apple_touch_icon);
    httpd_register_uri_handler(s_state.camera_httpd, &apple_touch_icon_plain);
    httpd_register_uri_handler(s_state.camera_httpd, &stream_compat);
    httpd_register_uri_handler(s_state.camera_httpd, &drive);
    ESP_LOGI(TAG, "camera http server started on port %u", camera_cfg.server_port);

    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port = camera_cfg.server_port + 1;
    stream_cfg.ctrl_port = camera_cfg.ctrl_port + 1;
    stream_cfg.max_open_sockets = 5;
    if (httpd_start(&s_state.stream_httpd, &stream_cfg) != ESP_OK) {
        httpd_stop(s_state.camera_httpd);
        s_state.camera_httpd = NULL;
        s_state.stream_httpd = NULL;
        return false;
    }

    httpd_uri_t stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
    };
    httpd_register_uri_handler(s_state.stream_httpd, &stream);
    ESP_LOGI(TAG, "stream http server started on port %u", stream_cfg.server_port);
    return true;
}

void RcCarApp_Start(void)
{
    esp_log_level_set("comm_wifi", ESP_LOG_INFO);
    comm_wifi_start();

    if (!comm_wifi_switch_to_ap_open(RC_CAR_AP_SSID)) {
        ESP_LOGE(TAG, "failed to start AP: %s", RC_CAR_AP_SSID);
        return;
    }
    if (!RcCarCamera_Init()) {
        ESP_LOGE(TAG, "camera init failed: %s", RcCarCamera_LastError());
        return;
    }
    if (!RcCarMotor_Init()) {
        ESP_LOGE(TAG, "motor init failed");
        return;
    }
    if (!s_state.ctrl_udp_task) {
        xTaskCreate(ctrl_udp_task, "rc_ctrl_udp", 4096, NULL, 5, &s_state.ctrl_udp_task);
    }
    if (!start_http_server()) {
        ESP_LOGE(TAG, "http server start failed");
        return;
    }

    apply_command(kRcCarMotorCmdStop);
    ESP_LOGI(TAG,
             "RC_CAR ready, AP=%s, control=http://192.168.4.1/ capture=http://192.168.4.1/capture stream=http://192.168.4.1:81/stream",
             RC_CAR_AP_SSID);
}

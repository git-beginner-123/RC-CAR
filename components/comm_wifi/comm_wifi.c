#include "comm_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

static const char *TAG = "comm_wifi";

static bool s_started = false;
static esp_netif_t *s_ap_netif = NULL;

static uint8_t choose_softap_channel(void)
{
    static const uint8_t kCandidates[] = {1, 6, 11};
    wifi_ap_record_t *records = NULL;
    uint16_t ap_count = 0;
    int scores[3] = {0, 0, 0};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed, fallback channel 1: %s", esp_err_to_name(err));
        (void)esp_wifi_stop();
        return 1;
    }
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        ESP_LOGI(TAG, "no nearby APs found, use channel 1");
        (void)esp_wifi_stop();
        return 1;
    }

    if (ap_count > 24) {
        ap_count = 24;
    }
    records = (wifi_ap_record_t *)heap_caps_malloc(sizeof(wifi_ap_record_t) * ap_count, MALLOC_CAP_8BIT);
    if (!records) {
        ESP_LOGW(TAG, "alloc scan records failed, fallback channel 1");
        (void)esp_wifi_stop();
        return 1;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        ESP_LOGW(TAG, "read scan records failed, fallback channel 1");
        free(records);
        (void)esp_wifi_stop();
        return 1;
    }

    for (uint16_t i = 0; i < ap_count; ++i) {
        uint8_t ch = records[i].primary;
        if (ch >= 1 && ch <= 3) {
            scores[0] += 100 + records[i].rssi;
        } else if (ch >= 4 && ch <= 8) {
            scores[1] += 100 + records[i].rssi;
        } else if (ch >= 9 && ch <= 13) {
            scores[2] += 100 + records[i].rssi;
        }
    }

    free(records);
    (void)esp_wifi_stop();

    int best_idx = 0;
    for (int i = 1; i < 3; ++i) {
        if (scores[i] < scores[best_idx]) {
            best_idx = i;
        }
    }
    ESP_LOGI(TAG, "softAP channel scores: ch1=%d ch6=%d ch11=%d, select=%u",
             scores[0], scores[1], scores[2], kCandidates[best_idx]);
    return kCandidates[best_idx];
}

static bool init_nvs_once(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

void comm_wifi_start(void)
{
    if (s_started) {
        return;
    }

    if (!init_nvs_once()) {
        ESP_LOGE(TAG, "nvs init failed");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_started = true;
}

void comm_wifi_stop(void)
{
    if (!s_started) {
        return;
    }

    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_started = false;
}

int comm_wifi_scan_top3(CommWifiAp *out, int cap)
{
    (void)out;
    (void)cap;
    return 0;
}

bool comm_wifi_connect_psk(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return false;
}

bool comm_wifi_is_connected(void)
{
    return false;
}

int comm_wifi_last_disconnect_reason(void)
{
    return 0;
}

bool comm_wifi_connect_saved(void)
{
    return false;
}

bool comm_wifi_get_connected_ssid(char *out_ssid, int cap)
{
    if (out_ssid && cap > 0) {
        out_ssid[0] = 0;
    }
    return false;
}

int comm_wifi_saved_credential_count(void)
{
    return 0;
}

bool comm_wifi_save_credential(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return false;
}

bool comm_wifi_connect_saved_any(uint32_t per_profile_timeout_ms, char *out_ssid, int out_cap)
{
    (void)per_profile_timeout_ms;
    if (out_ssid && out_cap > 0) {
        out_ssid[0] = 0;
    }
    return false;
}

bool comm_wifi_switch_to_ap_open(const char *ssid)
{
    uint8_t channel = 1;

    if (!s_started) {
        comm_wifi_start();
    }
    if (!s_started || !ssid || !ssid[0]) {
        return false;
    }

    channel = choose_softap_channel();

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = channel;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP started: %s channel=%u", ssid, channel);
    return true;
}

bool comm_wifi_switch_to_sta_only(void)
{
    return false;
}

bool comm_wifi_switch_to_sta_and_reconnect(void)
{
    return false;
}

bool comm_wifi_forget_saved_and_disconnect(void)
{
    return false;
}

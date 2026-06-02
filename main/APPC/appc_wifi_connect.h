#ifndef APPC_WIFI_CONNECT_H
#define APPC_WIFI_CONNECT_H
#include "ui.h"
#include "esp_wifi.h"

typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED_LOCAL, // Got IP, but no internet check yet
    WIFI_STATUS_CONNECTED_INTERNET, // Internet verified (Green)
    WIFI_STATUS_FAILED
} appc_wifi_status_t;

// Global variable to track status
//extern appc_wifi_status_t current_wifi_status;

// Function to update the UI based on status
void app_wifi_scan(void);
void app_wifi_scan_refresh(lv_event_t * e);
void app_wifi_connect(lv_event_t * e);
void appc_wifi_update_ui_status_set(appc_wifi_status_t status);
appc_wifi_status_t appc_wifi_update_ui_status_get(void);
void app_wifi_save_credentials(void);
esp_err_t app_nvs_load_credentials(char* ssid, char* password);
void app_wifi_auto_connect(wifi_ap_record_t * ap_info, uint16_t count);
void app_wifi_close(lv_event_t * e);
void app_wifi_enable_disable(lv_event_t * e);
void appc_wifi_ui_populate(void);

#endif
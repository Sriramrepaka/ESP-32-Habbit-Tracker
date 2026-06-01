#include "appc.h"
#include "esp_log.h"
#include "ui.h"
#include "esp_sntp.h"


static const char *TAG = "APPC_MAIN";

void appc_wifi_ui_timer_cb(lv_timer_t * timer) {
    // 1. Get the current status from your getter
    appc_wifi_status_t status = appc_wifi_update_ui_status_get();
    
    // Static variable to track the last color we set (prevents flickering)
    static appc_wifi_status_t last_status = -1;
    if (status == last_status) return; 
    last_status = status;

    // 2. Map the Enum to actual Colors
    lv_color_t color;
    switch (status) {
        case WIFI_STATUS_CONNECTING:
            color = lv_color_hex(0xFFFF00); // Yellow
            break;
        case WIFI_STATUS_CONNECTED_LOCAL:
            color = lv_color_hex(0xFF0000); // Red (Got IP, no internet yet)
            break;
        case WIFI_STATUS_CONNECTED_INTERNET:
            color = lv_color_hex(0x32B82D); // Green (Internet OK)
            
            break;
        case WIFI_STATUS_DISCONNECTED:
        default:
            color = lv_color_hex(0xF7F9FB); // Grey
            break;
    }

    // 3. Apply the style to the Panel
    if(ui_InternetIndicator != NULL) {
        lv_obj_set_style_bg_color(ui_InternetIndicator, color, LV_PART_MAIN | LV_STATE_DEFAULT);
        //lv_obj_set_style_bg_opa(ui_Panel6, 255, LV_PART_MAIN | LV_STATE_DEFAULT); 
        
        // Optional: Add a log to see the UI update in real-time
        ESP_LOGI(TAG, "Wifi colour updated: %d", status);
    }
}

void appc_update_clock_ui(void){
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Only update if time is actually set (year > 1970)
    if (timeinfo.tm_year > (1970 - 1900)) {
        char time_buf[16];
        
        // Format: HH:MM:SS
        strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
        //printf("Time = %s\n",time_buf);
        lv_label_set_text(ui_Clock_Number, time_buf);

        int minutes = timeinfo.tm_min; 
        uint16_t min_angle = minutes * 60;
        lv_img_set_angle(ui_Min, min_angle);

        int hours = timeinfo.tm_hour; 
        if (hours >= 12) hours -= 12; // Convert 24h to 12h format

        // (hours * 300) + (minutes * 5)
        uint16_t hour_angle = (hours * 300) + (timeinfo.tm_min * 5);
        lv_img_set_angle(ui_Hour, hour_angle);
    }
}

void appc_update_date_ui(void){
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (1970 - 1900)) {
        char date_buf[32];

        strftime(date_buf, sizeof(date_buf), "%A, %B %d", &timeinfo);
        //printf("Date = %s\n",date_buf);
        lv_label_set_text(ui_Date, date_buf);
    }  
}

void appc_clock_timer_cb(lv_timer_t * timer) {
    appc_update_clock_ui();
}

void appc_date_timer_cb(lv_timer_t * timer){
    appc_update_date_ui();  
}

void appc_init(void) {
    ESP_LOGI(TAG, "Initializing Application Controller Layer...");
    ESP_LOGI(TAG, "APPC Layer Started.");

    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    if (app_nvs_load_credentials(saved_ssid,saved_pass) == ESP_OK && strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Found saved SSID: %s. Auto-starting WiFi...", saved_ssid);
        
        // Force hardware to start so the Event Handler wakes up
        esp_err_t err = esp_wifi_start();
        if (err == ESP_OK || err == ESP_ERR_WIFI_STATE) {
            app_wifi_scan();
        }
    
    } else {
        ESP_LOGI(TAG, "No saved credentials. WiFi remains idle.");
    }
    //vTaskDelay(pdMS_TO_TICKS(5000));
    //app_sketch_init();

    // 1. Initialize WiFi Module logic
    // This will trigger the sync task that waits for Driver_Init to finish
    //app_wifi_scan(); 
    
    // 2. Initialize other modules later (e.g., app_audio_logic_init();)

    //Wifi connect button event
    lv_timer_create(appc_wifi_ui_timer_cb, 500, NULL);
    lv_timer_create(appc_clock_timer_cb, 1000, NULL);
    lv_timer_create(appc_date_timer_cb, 6*60*60*1000, NULL);
    //lv_label_set_text(ui_Label6, LV_SYMBOL_REFRESH);
    lv_obj_add_event_cb(ui_WifiConnectButton, app_wifi_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiRefreshButton, app_wifi_scan_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiCloseButton, app_wifi_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiEnableSwitch, app_wifi_enable_disable, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_SketchCloseBtn, appc_sketch_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_SketchSaveBtn, appc_sketch_save, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_NoteNameOkBtn2, appc_note_save, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_NoteNameCancelBtn, appc_note_cancel, LV_EVENT_CLICKED, NULL);

}
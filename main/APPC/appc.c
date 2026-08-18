#include "appc.h"
#include "esp_log.h"
#include "ui.h"
#include "esp_sntp.h"
#include "PCM5101.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
 


static const char *TAG = "APPC_MAIN";

static void sync_single_file(const char *src_path, const char *dst_path) {
    struct stat src_stat, dst_stat;

    if (stat(src_path, &src_stat) != 0) return;

    bool needs_update = false;
    if (stat(dst_path, &dst_stat) != 0) {
        ESP_LOGI(TAG, "New file detected: %s", dst_path);
        needs_update = true;
    } else if (src_stat.st_size != dst_stat.st_size) {
        ESP_LOGI(TAG, "Size mismatch for %s. Syncing...", dst_path);
        needs_update = true;
    }

    if (!needs_update) {
        ESP_LOGI(TAG, "Already up to date: %s", dst_path);
        return;
    }

    FILE *src = fopen(src_path, "rb");
    FILE *dst = fopen(dst_path, "wb");

    if (!src || !dst) {
        ESP_LOGE(TAG, "Failed to open handles: %s", dst_path);
        if (src) fclose(src);
        if (dst) fclose(dst);
        return;
    }

    size_t chunk_size = 2048;
    uint8_t *buffer = malloc(chunk_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Malloc failed");
        fclose(src);
        fclose(dst);
        return;
    }

    size_t bytes_read;
    size_t total_written = 0;
    while ((bytes_read = fread(buffer, 1, chunk_size, src)) > 0) {
        size_t bytes_written = fwrite(buffer, 1, bytes_read, dst);
        if (bytes_written < bytes_read) {
            ESP_LOGE(TAG, "Write error! SPIFFS partition full?");
            break;
        }
        total_written += bytes_written;
    }

    free(buffer);
    fclose(src);
    fclose(dst);

    ESP_LOGI(TAG, "Synced %s (%zu bytes)", dst_path, total_written);
}

void appc_sync_audio_folder(void) {
    const char *sd_dir_path = "/sdcard/Audio";
    DIR *dir = opendir(sd_dir_path);

    if (!dir) {
        ESP_LOGE(TAG, "Could not open SD audio directory.");
        return;
    }

    struct dirent *entry;
    
    // Increased from 128 to 300 to hold max prefix + 256-byte d_name
    char src_path[300];
    char dst_path[300];

    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files and navigation links (. and ..)
        if (entry->d_name[0] == '.') continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", sd_dir_path, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "/spiffs/%s", entry->d_name);

        sync_single_file(src_path, dst_path);
    }

    closedir(dir);
    ESP_LOGI(TAG, "Audio folder synchronization finished.");
}

void appc_sync_assets(void){
    appc_sync_audio_folder();
}

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
        appc_alarm_check_time(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
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

static void alarm_timer_cb(lv_timer_t * timer) {
    appc_alarm_process();
}

void appc_init(void) {
    ESP_LOGI(TAG, "Initializing Application Controller Layer...");
    ESP_LOGI(TAG, "APPC Layer Started.");

    appc_sync_assets();
    Play_Music("/spiffs", "bootup.mp3");
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

    //toggle_task_and_save(2026, 8, 7, 1);
    //toggle_task_and_save(2026, 8, 8, 2);
    //toggle_task_and_save(2026, 8, 9, 3);
    //toggle_task_and_save(2026, 8, 8, 1);
    //toggle_task_and_save(2026, 8, 7, 2);
    //toggle_task_and_save(2026, 8, 14, 1);

    //vTaskDelay(pdMS_TO_TICKS(5000));
    //app_sketch_init();

    // 1. Initialize WiFi Module logic
    // This will trigger the sync task that waits for Driver_Init to finish
    //app_wifi_scan(); 
    
    // 2. Initialize other modules later

    //Wifi connect button event
    lv_timer_create(appc_wifi_ui_timer_cb, 500, NULL);
    lv_timer_create(appc_clock_timer_cb, 1000, NULL);
    lv_timer_create(appc_date_timer_cb, 6*60*60*1000, NULL);
    lv_timer_create(alarm_timer_cb, 500, NULL);
    
    //WiFi events
    lv_obj_add_event_cb(ui_WifiConnectButton, app_wifi_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiRefreshButton, app_wifi_scan_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiCloseButton, app_wifi_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_settingsWifiPanel, appc_wifi_panel_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_WifiEnableSwitch, app_wifi_enable_disable, LV_EVENT_VALUE_CHANGED, NULL);

    //Note events
    lv_obj_add_event_cb(ui_SketchCloseBtn, appc_sketch_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_SketchSaveBtn, appc_sketch_save, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_NoteNameOkBtn, appc_note_save, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_NoteNameCancelBtn, appc_note_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_SketchViewDeleteBtn, appc_note_delete, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(ui_CalPrev, appc_task_cal_prev, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_CalNext, appc_task_cal_next, LV_EVENT_PRESSED, NULL);

    appc_sketch_init();
    appc_alarm_init();
    appc_tasks_init();
    
    //Play_Music("/sdcard/Audio", "SadaSiva.mp3");

}
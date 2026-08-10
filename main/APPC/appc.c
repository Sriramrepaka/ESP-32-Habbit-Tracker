#include "appc.h"
#include "esp_log.h"
#include "ui.h"
#include "esp_sntp.h"
#include "PCM5101.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "components/ui_comp_daycell.h" 


static const char *TAG = "APPC_MAIN";

void appc_sync_bootup_sound(void) {
    const char *sd_path = "/sdcard/Audio/bootup.mp3";
    const char *spiffs_path = "/spiffs/bootup.mp3";

    struct stat sd_stat, spiffs_stat;
    bool needs_update = false;

    // 1. Check if the SD card file even exists
    if (stat(sd_path, &sd_stat) != 0) {
        ESP_LOGI("SYNC", "No bootup.mp3 found on SD card. Skipping sync.");
        return;
    }

    // 2. Check if the SPIFFS file exists and compare sizes
    if (stat(spiffs_path, &spiffs_stat) != 0) {
        ESP_LOGI("SYNC", "Bootup sound not in flash yet. Updating...");
        needs_update = true;
    } else if (sd_stat.st_size != spiffs_stat.st_size) {
        ESP_LOGI("SYNC", "Bootup sound sizes differ (SD: %ld, Flash: %ld). Updating...", 
                 sd_stat.st_size, spiffs_stat.st_size);
        needs_update = true;
    }

    // 3. Perform the copy if needed
    if (needs_update) {
        FILE *src = fopen(sd_path, "rb");
        FILE *dst = fopen(spiffs_path, "wb");

        if (src == NULL || dst == NULL) {
            ESP_LOGE("SYNC", "Failed to open files for syncing!");
            if (src) fclose(src);
            if (dst) fclose(dst);
            return;
        }

        // MEMORY SAFE COPY: Transfer 2KB at a time so we don't crash the RAM
        size_t chunk_size = 2048;
        uint8_t *buffer = malloc(chunk_size);
        if (buffer == NULL) {
            ESP_LOGE("SYNC", "Failed to allocate memory for file copy.");
            fclose(src);
            fclose(dst);
            return;
        }

        size_t bytes_read;
        size_t total_written = 0;
        
        ESP_LOGI("SYNC", "Copying audio file to internal flash... Please wait.");
        
        while ((bytes_read = fread(buffer, 1, chunk_size, src)) > 0) {
            fwrite(buffer, 1, bytes_read, dst);
            total_written += bytes_read;
        }

        free(buffer);
        fclose(src);
        fclose(dst);

        ESP_LOGI("SYNC", "Sync complete! Wrote %zu bytes to flash.", total_written);
    } else {
        ESP_LOGI("SYNC", "Bootup sound is already up to date.");
    }
}

void appc_sync_assets(void){
    appc_sync_bootup_sound();
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

    appc_task_get_date();
    
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
    lv_obj_add_event_cb(ui_Productivity_, productivity_screen_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_add_event_cb(ui_CalPrev, appc_task_cal_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_CalNext, appc_task_cal_next, LV_EVENT_CLICKED, NULL);

    appc_sketch_init();
    appc_alarm_init();
    
    //Play_Music("/sdcard/Audio", "SadaSiva.mp3");

}
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "appc_wifi_connect.h"
#include "ui.h" // To access your list variable
#include "ui_events.h"
#include "Wireless.h"
#include "nvs_flash.h"
#include "nvs.h"

extern bool WiFi_Scan_Finish;
bool is_wifi_busy = false; // The safety lock
volatile bool trigger_wifi_ui_update = false;

extern char selected_ssid[32];
char password_buffer[64];

#define DEFAULT_SCAN_LIST_SIZE 15

static const char *TAG = "APPC_WIFI";

appc_wifi_status_t current_wifi_status = WIFI_STATUS_DISCONNECTED;

void appc_wifi_ui_populate(void) {

    uint16_t number = DEFAULT_SCAN_LIST_SIZE; // Set a reasonable limit
    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
    uint16_t ap_count = 0;

    // 1. Get the records from the driver
    esp_wifi_scan_get_ap_num(&ap_count);
    esp_err_t res = esp_wifi_scan_get_ap_records(&number, ap_info);

    if (res != ESP_OK || ap_count == 0) {
        ESP_LOGI(TAG, "No networks found.");
        return;
    }

    app_wifi_auto_connect(ap_info, number);
    
    if(lv_obj_get_screen(ui_uiWifiList) != lv_scr_act()) {
            ESP_LOGI(TAG,"Not on WIFI screen, Not populating UI");
            is_wifi_busy = false;
            return; 
    }

    lv_obj_clean(ui_uiWifiList);

    for (int i = 0; i < number; i++) {

        vTaskDelay(pdMS_TO_TICKS(1));

        lv_obj_t * new_btn = lv_btn_create(ui_uiWifiList);
        lv_obj_set_height(new_btn, 24);
        lv_obj_set_width(new_btn, lv_pct(100));
        lv_obj_set_align(new_btn, LV_ALIGN_CENTER);
        lv_obj_add_flag(new_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
        lv_obj_clear_flag(new_btn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
        lv_obj_add_flag(new_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(new_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(new_btn, lv_color_hex(0x0B0B0B), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_side(new_btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        lv_obj_set_style_shadow_width(new_btn, 0, 0);

        lv_obj_t * icon = lv_label_create(new_btn);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t * ssid_label = lv_label_create(new_btn);
        lv_label_set_text(ssid_label, (char *)ap_info[i].ssid);
        lv_obj_clear_flag(ssid_label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 40, 0);


        // 5. Attach the events you set in SquareLine
        // This connects it to your ui_events.c function
        lv_obj_add_event_cb(new_btn, ui_event_wifi_item_clicked, LV_EVENT_CLICKED, NULL);
    }

    is_wifi_busy = false;
    ESP_LOGI(TAG,"Finished wifi scan");
}

void wifi_wait_and_update_task(void * pvParameters) {
    extern bool WiFi_Scan_Finish; //

    // 1. Wait for the background hardware scan without freezing UI
    while (!WiFi_Scan_Finish) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 2. Schedule the UI update on the next LVGL cycle
    //lv_async_call((lv_async_cb_t)appc_wifi_ui_populate, NULL);
    trigger_wifi_ui_update = true;

    vTaskDelete(NULL);
}

void app_wifi_scan(void){
    ESP_LOGI(TAG,"Starting WIFI scan");
    // Create a background task so we don't freeze the UI while waiting
    xTaskCreate(wifi_wait_and_update_task, "wifi_ui_task", 4096, NULL, 5, NULL);
}

void app_wifi_scan_refresh(lv_event_t * e) {
    // If e is NOT null, we can check the code. 
    // If e IS null, it means we called it manually, so we just proceed.
    if (e != NULL) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_CLICKED){
            WiFi_Scan_Finish = 0;
            ESP_LOGI(TAG,"Starting manual WiFi Scan...");
            WIFI_Scan();
            app_wifi_scan();
        }
    }
    else if(e == NULL){
        ESP_LOGI(TAG,"Starting auto WiFi Scan...");
        WIFI_Scan();
        app_wifi_scan();
    }
}

void app_wifi_connect(lv_event_t * e) {

    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {

        
        const char * password = lv_textarea_get_text(ui_uiPassTextArea);
        strncpy(password_buffer, password, sizeof(password_buffer));
        ESP_LOGV(TAG,"Password is %s\n",password);

        wifi_config_t wifi_config = {0};
    
        // Copy the credentials into the config struct
        strncpy((char*)wifi_config.sta.ssid, selected_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

        ESP_LOGI(TAG,"Connecting to %s", selected_ssid);
        
        esp_wifi_disconnect(); // Disconnect from any previous session
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
        appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTING);

        //flags
        lv_textarea_set_text(ui_uiPassTextArea,"");
        lv_obj_add_flag(ui_uiPassPopup, LV_OBJ_FLAG_HIDDEN);
        _ui_screen_change(&ui_Settings, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_Settings_screen_init);
        lv_obj_add_state(ui_WifiEnableSwitch, LV_STATE_CHECKED);

    }
    
}

void appc_wifi_update_ui_status_set(appc_wifi_status_t status){
    current_wifi_status = status;
}

appc_wifi_status_t appc_wifi_update_ui_status_get(void){
    return current_wifi_status;
}

void app_wifi_save_credentials(void) {
    
    if( (strlen(selected_ssid) > 0) | (strlen(password_buffer) > 0) ){
        nvs_handle_t my_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
        if (err == ESP_OK) {
            nvs_set_str(my_handle, "saved_ssid", selected_ssid);
            nvs_set_str(my_handle, "saved_pass", password_buffer);
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "NVS: Credentials saved to flash. %s,%s",selected_ssid,password_buffer);
        }
    }
}

esp_err_t app_nvs_load_credentials(char* ssid, char* password) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // 1. Open NVS handle. Use the same namespace as your "save" function!
    err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW("NVS", "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    // 2. Read SSID
    size_t ssid_size = sizeof(selected_ssid); // Buffer size
    err = nvs_get_str(my_handle, "saved_ssid", ssid, &ssid_size);
    if (err != ESP_OK) {
        ESP_LOGW("NVS", "No SSID saved in NVS");
    }

    // 3. Read Password
    size_t pass_size = sizeof(password_buffer);
    err = nvs_get_str(my_handle, "saved_pass", password, &pass_size);
    if (err != ESP_OK) {
        ESP_LOGW("NVS", "No Password saved in NVS");
    }

    // 4. Close handle
    nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

void app_wifi_auto_connect(wifi_ap_record_t * ap_info, uint16_t count) {
    //nvs_handle_t my_handle;
    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    
    esp_err_t res = app_nvs_load_credentials(saved_ssid,saved_pass);
    if (res != ESP_OK) return;

    // 2. Look for the saved SSID in the scan results
    bool found = false;
    printf("SSID Saved %s\n",saved_ssid);
    for (int i = 0; i < count; i++) {
        printf("SSID %s\n",ap_info[i].ssid);
        if (strcmp((char *)ap_info[i].ssid, saved_ssid) == 0) {
            found = true;
            break;
        }
    }

    // 3. Only connect if it was actually found nearby
    if (found) {
        ESP_LOGI(TAG, "Saved SSID '%s' found nearby! Connecting...", saved_ssid);
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, saved_pass, sizeof(wifi_config.sta.password));

        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
        appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTING);
        
        // Update the UI Switch to 'Checked' to show we are active
        lv_obj_add_state(ui_WifiEnableSwitch, LV_STATE_CHECKED);
    } else {
        ESP_LOGI(TAG, "Saved SSID not in range. Auto-connect aborted.");
    }
}

void app_wifi_close(lv_event_t * e) {

    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED){

        // if(is_wifi_busy) {
        //     ESP_LOGW(TAG, "Cannot close yet: WiFi is busy scanning/populating.");
        //     // Optional: Show a "Please wait" toast or message
        //     return; 
        // }

        switch ((appc_wifi_update_ui_status_get()))
        {
            case WIFI_STATUS_FAILED:
                lv_obj_clear_state(ui_WifiEnableSwitch, LV_STATE_CHECKED);
                break;
            case WIFI_STATUS_DISCONNECTED:
                lv_obj_clear_state(ui_WifiEnableSwitch, LV_STATE_CHECKED);
                break;
            default:
                lv_obj_add_state(ui_WifiEnableSwitch, LV_STATE_CHECKED);
        }
        is_wifi_busy = false;
    }
}

void app_deferred_scan_cb(lv_timer_t * t) {
    app_wifi_scan();
}

void app_wifi_enable_disable(lv_event_t * e) {

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);

    

    if(code == LV_EVENT_VALUE_CHANGED){

        if(is_wifi_busy) {
        
            ESP_LOGW("WIFI", "Busy! Ignoring toggle.");
            // Optional: Snap the switch back to its previous visual state
            if(lv_obj_has_state(obj, LV_STATE_CHECKED)) 
                lv_obj_clear_state(obj, LV_STATE_CHECKED);
            else 
                lv_obj_add_state(obj, LV_STATE_CHECKED);
            return;

        }

        is_wifi_busy = true; // LOCK

        if(lv_obj_has_state(obj, LV_STATE_CHECKED)){

            ESP_LOGI(TAG, "Switching ON: Re-enabling WiFi...");

            _ui_screen_change(&ui_Wifimenu, LV_SCR_LOAD_ANIM_MOVE_LEFT, 500, 0, &ui_Wifimenu_screen_init);

            esp_err_t err = esp_wifi_start();
            printf("WiFi Start Result: %s\n", esp_err_to_name(err));
            //app_wifi_scan_refresh(NULL);
            if (err == ESP_OK || err == ESP_ERR_WIFI_STATE) {
        
                //vTaskDelay(pdMS_TO_TICKS(100)); 
                
                if(is_wifi_busy) { 
                    ESP_LOGI(TAG, "Event didn't fire (Already Started). Manual Scan trigger.");
                    lv_timer_t * timer = lv_timer_create(app_deferred_scan_cb, 500, NULL);
                    lv_timer_set_repeat_count(timer, 1);
                }
            }
        }
        else{
            ESP_LOGI(TAG, "Switching OFF: Disabling WiFi...");

            // 1. Stop hardware
            esp_wifi_disconnect();
            esp_wifi_stop();

            // 2. Clear UI
            if(ui_uiWifiList != NULL) {
                lv_obj_clean(ui_uiWifiList);
            }

            // 3. Update Status Dot to Grey
            appc_wifi_update_ui_status_set(WIFI_STATUS_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(500));
            is_wifi_busy = false; // UNLOCK
        }
    }

}
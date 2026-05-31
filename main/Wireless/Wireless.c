#include "Wireless.h"
#include "appc_wifi_connect.h"
#include "appc.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "arpa/inet.h"   // This usually handles inet_ntoa/inet_pton
#include "ping/ping_sock.h"
#include "esp_sntp.h"

uint16_t BLE_NUM = 0;
uint16_t WIFI_NUM = 0;
bool Scan_finish = 0;

static const char *TAG = "WIFI";

bool WiFi_Scan_Finish = 0;
bool BLE_Scan_Finish = 0;

extern char selected_ssid[33];
extern bool is_wifi_busy;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

void Wireless_Init(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    // WiFi
    xTaskCreatePinnedToCore(
        WIFI_Init, 
        "WIFI task",
        4096, 
        NULL, 
        1, 
        NULL, 
        0);
    // // BLE
    // xTaskCreatePinnedToCore(
    //     BLE_Init, 
    //     "BLE task",
    //     4096, 
    //     NULL, 
    //     2, 
    //     NULL, 
    //     0);
}

void WIFI_Init(void *arg)
{
    esp_netif_init();                                                     
    esp_event_loop_create_default();                                      
    esp_netif_create_default_wifi_sta();                                 
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();                 
    esp_wifi_init(&cfg);                                      
    esp_wifi_set_mode(WIFI_MODE_STA);              
    esp_wifi_start();                            

    WIFI_NUM = WIFI_Scan();
    printf("WIFI:%d\r\n",WIFI_NUM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&event_handler,NULL,NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&event_handler,NULL,NULL));

    vTaskDelete(NULL);
}
uint16_t WIFI_Scan(void)
{
    uint16_t ap_count = 0;
    

    esp_err_t scan_err = esp_wifi_scan_start(NULL, true);
    if (scan_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: 0x%x", scan_err);
        return 0; 
    }

    esp_err_t err = esp_wifi_scan_get_ap_num(&ap_count);

    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "Scan aborted: WiFi was stopped by user.");
        return 0; // Exit safely! No crash!
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed with error: 0x%x", err);
        return 0;
    }

    esp_wifi_scan_stop();
    WiFi_Scan_Finish =1;
    if(BLE_Scan_Finish == 1)
        Scan_finish = 1;
    return ap_count;
}


#define GATTC_TAG "GATTC_TAG"
#define SCAN_DURATION 5  
#define MAX_DISCOVERED_DEVICES 100 

typedef struct {
    uint8_t address[6];
    bool is_valid;
} discovered_device_t;

static discovered_device_t discovered_devices[MAX_DISCOVERED_DEVICES];
static size_t num_discovered_devices = 0;
static size_t num_devices_with_name = 0; 

static bool is_device_discovered(const uint8_t *addr) {
    for (size_t i = 0; i < num_discovered_devices; i++) {
        if (memcmp(discovered_devices[i].address, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void add_device_to_list(const uint8_t *addr) {
    if (num_discovered_devices < MAX_DISCOVERED_DEVICES) {
        memcpy(discovered_devices[num_discovered_devices].address, addr, 6);
        discovered_devices[num_discovered_devices].is_valid = true;
        num_discovered_devices++;
    }
}

static bool extract_device_name(const uint8_t *adv_data, uint8_t adv_data_len, char *device_name, size_t max_name_len) {
    size_t offset = 0;
    while (offset < adv_data_len) {
        if (adv_data[offset] == 0) break; 

        uint8_t length = adv_data[offset];
        if (length == 0 || offset + length > adv_data_len) break; 

        uint8_t type = adv_data[offset + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            if (length > 1 && length - 1 < max_name_len) {
                memcpy(device_name, &adv_data[offset + 2], length - 1);
                device_name[length - 1] = '\0'; 
                return true;
            } else {
                return false;
            }
        }
        offset += length + 1;
    }
    return false;
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    static char device_name[100]; 

    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                if (!is_device_discovered(param->scan_rst.bda)) {
                    add_device_to_list(param->scan_rst.bda);
                    BLE_NUM++; 

                    if (extract_device_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len, device_name, sizeof(device_name))) {
                        num_devices_with_name++;
                        // printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: %s\n        RSSI: %d\r\n",
                        //          param->scan_rst.bda[0], param->scan_rst.bda[1],
                        //          param->scan_rst.bda[2], param->scan_rst.bda[3],
                        //          param->scan_rst.bda[4], param->scan_rst.bda[5],
                        //          device_name, param->scan_rst.rssi);
                        // printf("\r\n");
                    } else {
                        // printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: Unknown\n        RSSI: %d\r\n",
                        //          param->scan_rst.bda[0], param->scan_rst.bda[1],
                        //          param->scan_rst.bda[2], param->scan_rst.bda[3],
                        //          param->scan_rst.bda[4], param->scan_rst.bda[5],
                        //          param->scan_rst.rssi);
                        // printf("\r\n");
                    }
                }
            }
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(GATTC_TAG, "Scan complete. Total devices found: %d (with names: %d)", BLE_NUM, num_devices_with_name);
            break;
        default:
            break;
    }
}

void BLE_Init(void *arg)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);                                            
    if (ret) {
        printf("%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));        
        return;}
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);                                            
    if (ret) {
        printf("%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));            
        return;}
    ret = esp_bluedroid_init();                                                                 
    if (ret) {
        printf("%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));               
        return;}
    ret = esp_bluedroid_enable();                                                               
    if (ret) {
        printf("%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));             
        return;}

    //register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);                                            
    if (ret){
        printf("%s gap register error, error code = %x\n", __func__, ret);                      
        return;
    }
    BLE_Scan();
    // while(1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(150));
    // }
    
    vTaskDelete(NULL);

}

uint16_t BLE_Scan(void)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,     
        .scan_window = 0x30,        
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));

    printf("Starting BLE scan...\n");
    ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION));
    
    // Set scanning duration
    vTaskDelay(SCAN_DURATION * 1000 / portTICK_PERIOD_MS);
    
    printf("Stopping BLE scan...\n");
    // ESP_ERROR_CHECK(esp_ble_gap_stop_scanning());
    ESP_ERROR_CHECK(esp_ble_dtm_stop());
    BLE_Scan_Finish = 1;
    if(WiFi_Scan_Finish == 1)
        Scan_finish = 1;
    return BLE_NUM;
}

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    
    // Using ipaddr_ntoa is the safer, native ESP-IDF way to print IPs
    printf("LOG: Ping success from %s! Time: %ldms. Dot to GREEN.\n", ipaddr_ntoa(&target_addr), elapsed_time);
    
    appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTED_INTERNET);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    printf("LOG: Ping timeout. No Internet. Dot stays RED.\n");
    appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTED_LOCAL);
}

void internet_check_task(void* p) {
    vTaskDelay(pdMS_TO_TICKS(1000)); 

    ip_addr_t target_addr;
    // This is a much cleaner way in ESP-IDF to set a string IP
    ip4addr_aton("8.8.8.8", ip_2_ip4(&target_addr));
    target_addr.type = IPADDR_TYPE_V4;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 1; 

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .cb_args = NULL
    };

    esp_ping_handle_t ping;
    esp_ping_new_session(&ping_config, &cbs, &ping);
    esp_ping_start(ping);

    vTaskDelete(NULL);
}

void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI("SNTP", "Notification: Time has been synchronized!");
    
    // Call your UI update functions here
    appc_update_clock_ui();
    ESP_LOGI(TAG,"Time Updated");
    appc_update_date_ui();
    ESP_LOGI(TAG,"Date Updated");
}

void start_sntp(void) {

    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already running, skipping initialization.");
        return; 
    }

    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org"); // Standard NTP server

    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    esp_sntp_init();

    // Set your Timezone (Example: IST is UTC+5:30)
    setenv("TZ", "UTC-2", 1); 
    tzset();
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    // 1. CHECK THE DEPARTMENT (BASE)
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                printf("LOG: WiFi Started\n");
                is_wifi_busy = false;
                app_wifi_scan_refresh(NULL);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                printf("LOG: Connected to Radio\n"); //red dot
                is_wifi_busy = false;
                appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTING);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                printf("LOG: Disconnected\n"); //grey dot
                is_wifi_busy = false;
                appc_wifi_update_ui_status_set(WIFI_STATUS_DISCONNECTED);
                break;
            case WIFI_EVENT_STA_STOP:
                printf("LOG: WiFi Stopped\n");
                is_wifi_busy = false;
                break;
        }
    } 
    // 2. CHECK THE NETWORKING DEPARTMENT
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            printf("LOG: Got IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
            appc_wifi_update_ui_status_set(WIFI_STATUS_CONNECTED_LOCAL);
            app_wifi_save_credentials();
            // 2. Create the task to check for Green
            xTaskCreate(internet_check_task, "net_check", 4096, NULL, 5, NULL);
            start_sntp();
    }
}


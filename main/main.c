#include "ST7789.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "SD_MMC.h"
#include "Wireless.h"
#include "LVGL_Example.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"
#include "PCM5101.h"
#include "ui.h"
#include "appc.h"
#include "esp_task_wdt.h"
#include "esp_spiffs.h"

void Driver_Loop(void *parameter)
{
    Wireless_Init();
    while(1)
    {
        QMI8658_Loop();
        PCF85063_Loop();
        BAT_Get_Volts();
        PWR_Loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}
void Driver_Init(void)
{
    PWR_Init();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();
    QMI8658_Init();
    Flash_Searching();
    xTaskCreatePinnedToCore(
        Driver_Loop, 
        "Other Driver task",
        4096, 
        NULL, 
        3, 
        NULL, 
        0);
}
void SPIFFS_Init(void) {
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true 
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        printf("Failed to mount SPIFFS (%s)\n", esp_err_to_name(ret));
    } else {
        printf("SPIFFS mounted successfully!\n");
    }
}

void app_main(void)
{
    Driver_Init();
    SPIFFS_Init();
    printf("Here while booting 1\n");
    SD_Init();
    LCD_Init();
    Audio_Init();
    LVGL_Init();   // returns the screen object
    ui_init();
    appc_init();
    appc_sketch_init();
    //appc_build_calendar(3,31);
    printf("Here while booting 2\n");


/********************* Demo *********************/
    // Lvgl_Example1();
    // lv_demo_widgets();
    // lv_demo_keypad_encoder();
    // lv_demo_benchmark();
    // lv_demo_stress();
    // lv_demo_music();
    // demo_wifi();
    
    while (1) {

        lv_timer_handler(); 
        
        vTaskDelay(pdMS_TO_TICKS(10));
        
    }
}







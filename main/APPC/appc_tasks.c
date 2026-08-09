#include "appc_sketch.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h> // Added for mkdir() and stat()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include "appc_tasks.h"

static const char *TAG = "APPC_TASK";

int cur_day;
int cur_day_week;
int cur_month;
int cur_year;
int cur_month_days;
int cur_month_offset;

static day_task_status_t current_month_tasks[31];

// 1. Check for leap year
bool is_leap_year(int year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

// 2. Get total days in a month (1 - 12)
int get_days_in_month(int year, int month) {
    static const int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days_per_month[month];
}

// 3. Get first day offset of the month
// Sunday start (0 = Sun, 1 = Mon, ..., 6 = Sat)
int get_month_start_offset(int year, int month) {
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1; // tm_mon is 0-indexed
    t.tm_mday = 1;         // Target the 1st of the month

    mktime(&t); // Populates t.tm_wday automatically
    
    return t.tm_wday; 
}

void appc_task_get_date(void){
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (1970 - 1900)) {
        
        cur_year = timeinfo.tm_year + 1900;
        cur_month = timeinfo.tm_mon + 1;
        cur_day = timeinfo.tm_mday;
        cur_day_week = timeinfo.tm_wday;
        cur_month_days = get_days_in_month(cur_year,cur_month);
        cur_month_offset = get_month_start_offset(cur_year,cur_month);
    }  
}

// 1. Function to build the 35 lightweight cells
void appc_build_lightweight_calendar(lv_obj_t * parent, int start_offset, int total_days) {
    // Clear any previous cells if calling again
    lv_obj_clean(parent);

    for (int i = 0; i < 35; i++) {
        // --- 1. Parent Cell Container ---
        lv_obj_t * cell = lv_obj_create(parent);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 30, 30);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        // --- 2. Outer Red Ring (Task 1) ---
        lv_obj_t * r1 = lv_obj_create(cell);
        lv_obj_remove_style_all(r1); // Remove default panel background/padding
        lv_obj_set_size(r1, 28, 28);
        lv_obj_set_style_radius(r1, 14, 0);
        // #FFFF00 
        lv_obj_set_style_border_color(r1, lv_color_hex(0xFFFF00), 0);
        lv_obj_set_style_border_width(r1, 2, 0);
        lv_obj_set_style_bg_opa(r1, LV_OPA_TRANSP, 0); // Keep center transparent
        lv_obj_center(r1);
        lv_obj_add_flag(r1, LV_OBJ_FLAG_HIDDEN); // Hide by default

        // --- 3. Middle Blue Ring (Task 2) ---
        lv_obj_t * r2 = lv_obj_create(cell);
        lv_obj_remove_style_all(r2);
        lv_obj_set_size(r2, 22, 22);
        lv_obj_set_style_radius(r2, 11, 0);
        // #2DCBD7 
        lv_obj_set_style_border_color(r2, lv_color_hex(0x2DCBD7), 0);
        lv_obj_set_style_border_width(r2, 2, 0);
        lv_obj_set_style_bg_opa(r2, LV_OPA_TRANSP, 0);
        lv_obj_center(r2);
        lv_obj_add_flag(r2, LV_OBJ_FLAG_HIDDEN); // Hide by default

        // --- 4. Inner Green Ring (Task 3) ---
        lv_obj_t * r3 = lv_obj_create(cell);
        lv_obj_remove_style_all(r3);
        lv_obj_set_size(r3, 16, 16);
        lv_obj_set_style_radius(r3, 8, 0);
        // #FA080E
        lv_obj_set_style_border_color(r3, lv_color_hex(0xFA080E), 0);
        lv_obj_set_style_border_width(r3, 2, 0);
        lv_obj_set_style_bg_opa(r3, LV_OPA_TRANSP, 0);
        lv_obj_center(r3);
        lv_obj_add_flag(r3, LV_OBJ_FLAG_HIDDEN); // Hide by default

        // --- 5. Date Text Label ---
        lv_obj_t * lbl = lv_label_create(cell);
        // 1. Set Text Color (e.g., Hex color #FFFFFF)
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

        // 2. Set Font Size (e.g., Montserrat 12pt)
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

        int day_num = i - start_offset + 1;

        if (day_num > 0 && day_num <= total_days) {
            lv_label_set_text_fmt(lbl, "%d", day_num);
        } else {
            lv_label_set_text(lbl, ""); // Keep leading/trailing padding days blank
        }
        lv_obj_center(lbl);
    }

    switch (cur_month)
    {
    case 1:
        lv_label_set_text(ui_CalMonth,"January");
        break;
    case 2:
        lv_label_set_text(ui_CalMonth,"February");
        break;
    case 3:
        lv_label_set_text(ui_CalMonth,"March");
        break;
    case 4:
        lv_label_set_text(ui_CalMonth,"April");
        break;
    case 5:
        lv_label_set_text(ui_CalMonth,"May");
        break;
    case 6:
        lv_label_set_text(ui_CalMonth,"June");
        break;
    case 7:
        lv_label_set_text(ui_CalMonth,"July");
        break;
    case 8:
        lv_label_set_text(ui_CalMonth,"August");
        break;
    case 9:
        lv_label_set_text(ui_CalMonth,"September");
        break;
    case 10:
        lv_label_set_text(ui_CalMonth,"October");
        break;
    case 11:
        lv_label_set_text(ui_CalMonth,"November");
        break;
    case 12:
        lv_label_set_text(ui_CalMonth,"December");
        break;

    default:
        lv_label_set_text(ui_CalMonth,"N/A");
        break;
    }
}

void load_month_from_sd(int year, int month) {
    char filepath[32];
    snprintf(filepath, sizeof(filepath), "/sdcard/tasks/%04d_%02d.bin", year, month);

    FILE *f = fopen(filepath, "rb");
    if (f) {
        fread(current_month_tasks, sizeof(day_task_status_t), 31, f);
        fclose(f);
    } else {
        // File doesn't exist yet for this month -> clear data array
        memset(current_month_tasks, 0, sizeof(current_month_tasks));
    }
}

void appc_set_day_tasks(lv_obj_t * parent, int day_num, int start_offset, bool task1, bool task2, bool task3) {
    int cell_index = day_num + start_offset - 1;
    lv_obj_t * cell = lv_obj_get_child(parent, cell_index);
    if (!cell) return;

    // Get child ring handles by their creation index
    lv_obj_t * r1 = lv_obj_get_child(cell, 0); // Red
    lv_obj_t * r2 = lv_obj_get_child(cell, 1); // Blue
    lv_obj_t * r3 = lv_obj_get_child(cell, 2); // Green

    // Toggle visibility based on state
    if (task1)   lv_obj_clear_flag(r1, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(r1, LV_OBJ_FLAG_HIDDEN);

    if (task2)  lv_obj_clear_flag(r2, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(r2, LV_OBJ_FLAG_HIDDEN);

    if (task3) lv_obj_clear_flag(r3, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(r3, LV_OBJ_FLAG_HIDDEN);
}

void productivity_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SCREEN_LOADED) {
        // 1. Read history from SD Card for target month
        load_month_from_sd(cur_year, cur_month); 

        // 2. Build the 35 lightweight cells
        appc_build_lightweight_calendar(ui_CalenderHolder, cur_month_offset, cur_month_days);

        // 3. Update ring visibilities based on loaded SD data
        for (int day = 1; day <= 31; day++) {
            day_task_status_t status = current_month_tasks[day - 1];
            appc_set_day_tasks(ui_CalenderHolder, day, 3, status.task_one, status.task_two, status.task_three);
        }
    } 
    else if (code == LV_EVENT_SCREEN_UNLOADED) {
        // Free LVGL heap memory when navigating away
        lv_obj_clean(ui_CalenderHolder);
    }
}

void toggle_task_and_save(int year, int month, int day, int task_num) {

    load_month_from_sd(year,month);

    ESP_LOGI(TAG,"Trying to save task.....");

    // 1. Update RAM array
    if (task_num == 1) current_month_tasks[day - 1].task_one ^= 1;
    if (task_num == 2) current_month_tasks[day - 1].task_two ^= 1;
    if (task_num == 3) current_month_tasks[day - 1].task_three ^= 1;

    // 2. Write updated month data to SD Card
    char filepath[32];
    snprintf(filepath, sizeof(filepath), "/sdcard/tasks/%04d_%02d.bin", year, month);

    struct stat st = {0};
    if (stat("/sdcard/tasks", &st) == -1) {
        ESP_LOGI(TAG, "Tasks directory does not exist. Creating it now...");
        mkdir("/sdcard/tasks", 0777);
    }

    FILE *f = fopen(filepath, "wb");
    if (f) {
        fwrite(current_month_tasks, sizeof(day_task_status_t), 31, f);
        fclose(f);
        ESP_LOGI(TAG,"Successfully saved task.....");
    }
    else{
        ESP_LOGI(TAG,"Failed to save task.....");
    }
}



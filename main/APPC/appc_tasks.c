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
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "APPC_TASK";

typedef struct {
    bool    is_pomodoro;  // true / false
    uint8_t hour;         // 0 - 23
    uint8_t minute;       // 0 - 59
} habit_task_t;

static habit_task_t g_tasks[3] = {
    {false, 0, 30}, // Task 1 default: 30 mins
    {false, 1, 0},  // Task 2 default: 1 hour
    {true,  0, 25}  // Task 3 default: 25 mins (Pomodoro)
};

static uint8_t s_selected_task_index = 0;

static uint32_t s_total_work_remaining_sec = 0; // Overall target work time
static uint32_t s_current_interval_sec     = 0; // Countdown interval active on screen
static bool     s_is_pomo_break             = false; // true = Break mode, false = Work mode
static lv_timer_t * s_task_timer           = NULL;

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

        ESP_LOGI(TAG,"Date updated succesfully for calender generation");
    }  
}

void appc_save_all_tasks(void) {
    nvs_handle_t handle;
    if (nvs_open("tracker_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        // Save the entire array of 3 tasks in one call
        nvs_set_blob(handle, "tasks_data", g_tasks, sizeof(g_tasks));
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG,"Tasks settings updated and saved");
}

void appc_load_all_tasks(void) {
    nvs_handle_t handle;
    if (nvs_open("tracker_cfg", NVS_READONLY, &handle) == ESP_OK) {
        size_t required_size = sizeof(g_tasks);
        nvs_get_blob(handle, "tasks_data", g_tasks, &required_size);
        nvs_close(handle);
    }
}

// Update UI controls to show values of the currently selected task index
static void appc_sync_settings_ui(uint8_t task_idx) {
    if (task_idx > 2) return;

    if (g_tasks[task_idx].is_pomodoro) {
        lv_obj_add_state(ui_PomodoroCheckbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_PomodoroCheckbox, LV_STATE_CHECKED);
    }

    lv_roller_set_selected(ui_RollerTaskHr, g_tasks[task_idx].hour, LV_ANIM_OFF);

    static const uint8_t roller_values[] = {0, 10, 15, 30, 45};
    size_t count = sizeof(roller_values) / sizeof(roller_values[0]);

    for (size_t i = 0; i < count; i++) {
        if (roller_values[i] == g_tasks[task_idx].minute) {
            lv_roller_set_selected(ui_RollerTaskMin, (uint16_t)i, LV_ANIM_OFF);
            return;
        }
    }
}

// Event 1: Triggered when user selects a different task in the Dropdown
void ui_TaskDropdown_event_cb(lv_event_t * e) {
    uint8_t selected_idx = (uint8_t)lv_dropdown_get_selected(ui_TaskDropdown);
    appc_sync_settings_ui(selected_idx);
}

// Event 2: Triggered when user clicks the final SET button
void ui_SetButton_event_cb(lv_event_t * e) {
    uint8_t selected_idx = (uint8_t)lv_dropdown_get_selected(ui_TaskDropdown);

    // Write widget values into the array element for this specific task
    g_tasks[selected_idx].is_pomodoro = lv_obj_has_state(ui_PomodoroCheckbox, LV_STATE_CHECKED);
    g_tasks[selected_idx].hour        = (uint8_t)lv_roller_get_selected(ui_RollerTaskHr);

    char buf[8];

    // Copy selected string (e.g., "15") into buf
    lv_roller_get_selected_str(ui_RollerTaskMin, buf, sizeof(buf));

    // Convert string to integer (e.g., 15)
    uint8_t actual_val = (uint8_t)atoi(buf);
    g_tasks[selected_idx].minute      = actual_val;

    // Persist all 3 tasks to Flash memory
    appc_save_all_tasks();

    // Hide panels
    lv_obj_add_flag(ui_TaskTimeSet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_TaskSelectPanel, LV_OBJ_FLAG_HIDDEN);
}

static void task_label_long_press_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * label = lv_event_get_target(e);

    if (code == LV_EVENT_LONG_PRESSED) {
        // Retrieve the task index stored on this object
        s_selected_task_index = (uint8_t)(uintptr_t)lv_obj_get_user_data(label);

        // Pre-fill the initial time labels on TaskOnGoingPanel before starting
        habit_task_t * task = &g_tasks[s_selected_task_index];
        
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d", task->hour);
        lv_label_set_text(ui_TaskTimeHourLabel, buf);

        snprintf(buf, sizeof(buf), "%02d", task->minute);
        lv_label_set_text(ui_TaskTimeMinLabel, buf);

        lv_label_set_text(ui_TaskTimeSecLabel, "00");

        // Unhide the panel
        lv_obj_clear_flag(ui_TaskOnGoingPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_TaskOnGoingPanel);
    }
}


void appc_setup_clock_task_labels(void) {
    // Label 1 setup
    lv_obj_add_flag(ui_Task1min, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ui_Task1min, (void*)(uintptr_t)0);
    lv_obj_add_event_cb(ui_Task1min, task_label_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    // Label 2 setup
    lv_obj_add_flag(ui_Task2min, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ui_Task2min, (void*)(uintptr_t)1);
    lv_obj_add_event_cb(ui_Task2min, task_label_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    // Label 3 setup
    lv_obj_add_flag(ui_Task3min, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ui_Task3min, (void*)(uintptr_t)2);
    lv_obj_add_event_cb(ui_Task3min, task_label_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
}


static void update_time_labels(uint32_t total_sec) {
    uint8_t h = total_sec / 3600;
    uint8_t m = (total_sec % 3600) / 60;
    uint8_t s = total_sec % 60;

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", h);
    lv_label_set_text(ui_TaskTimeHourLabel, buf);

    snprintf(buf, sizeof(buf), "%02d", m);
    lv_label_set_text(ui_TaskTimeMinLabel, buf);

    snprintf(buf, sizeof(buf), "%02d", s);
    lv_label_set_text(ui_TaskTimeSecLabel, buf);
}

static void finish_task_timer(void) {
    if (s_task_timer != NULL) {
        lv_timer_del(s_task_timer);
        s_task_timer = NULL;
    }
    // Hide the ongoing task panel
    lv_obj_add_flag(ui_TaskOnGoingPanel, LV_OBJ_FLAG_HIDDEN);
}

static void task_timer_cb(lv_timer_t * timer) {
    if (s_current_interval_sec > 0) {
        s_current_interval_sec--;

        // Only decrement the total work budget during active work sessions
        if (!s_is_pomo_break && s_total_work_remaining_sec > 0) {
            s_total_work_remaining_sec--;
        }

        update_time_labels(s_current_interval_sec);
    } else {

        Play_Music("/spiffs", "timer.mp3");

        // Current interval (work or break) reached 00:00:00
        habit_task_t * task = &g_tasks[s_selected_task_index];

        if (task->is_pomodoro) {
            if (!s_is_pomo_break) {
                // Just finished a WORK block
                if (s_total_work_remaining_sec > 0) {
                    // Work time still remains -> Start a 5-minute BREAK
                    s_is_pomo_break = true;
                    s_current_interval_sec = 5 * 60; // 5-minute break

                    lv_label_set_text(ui_TaskOnGoingLabel, "Pause");
                    update_time_labels(s_current_interval_sec);
                } else {
                    // Work budget exhausted -> Complete task!
                    finish_task_timer();
                }
            } else {
                // Just finished a BREAK block -> Switch back to WORK
                s_is_pomo_break = false;

                // Grab next work chunk: 25 mins or whatever work time remains
                uint32_t next_work_sec = (s_total_work_remaining_sec > (25 * 60)) 
                                         ? (25 * 60) 
                                         : s_total_work_remaining_sec;

                if (next_work_sec > 0) {
                    s_current_interval_sec = next_work_sec;

                    lv_label_set_text(ui_TaskOnGoingLabel, "Hustle");

                    update_time_labels(s_current_interval_sec);
                } else {
                    finish_task_timer();
                }
            }
            update_time_labels(s_current_interval_sec);
        } else {
            // Standard non-pomodoro timer completed
            finish_task_timer();
        }
    }
}

// Event when TaskOnGoingBtn is clicked to launch the task
void ui_TaskOnGoingBtn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        habit_task_t * task = &g_tasks[s_selected_task_index];

        // 1. Store total target work duration (e.g. 1 hour = 3600 seconds)
        s_total_work_remaining_sec = (task->hour * 3600) + (task->minute * 60);
        s_is_pomo_break = false;

        if (s_total_work_remaining_sec == 0) return;

        lv_label_set_text(ui_TaskOnGoingLabel, "Hustle");

        if (task->is_pomodoro) {
            // First work chunk is 25 mins (or less if target < 25 mins)
            s_current_interval_sec = (s_total_work_remaining_sec > (25 * 60)) 
                                     ? (25 * 60) 
                                     : s_total_work_remaining_sec;
        } else {
            // Standard timer counts down the full time directly
            s_current_interval_sec = s_total_work_remaining_sec;
        }

        update_time_labels(s_current_interval_sec);

        // Delete active timer if one is running
        if (s_task_timer != NULL) {
            lv_timer_del(s_task_timer);
        }

        // Create new 1-second interval timer
        s_task_timer = lv_timer_create(task_timer_cb, 1000, NULL);
    }
}

// 1. Function to build the 42 lightweight cells
void appc_build_lightweight_calendar(lv_obj_t * parent, int start_offset, int total_days) {

    ESP_LOGI(TAG,"Rendering calender at screen load");

    // Clear any previous cells if calling again
    lv_obj_clean(parent);

    for (int i = 0; i < 42; i++) {
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

    lv_label_set_text_fmt(ui_CalYear, "%d", cur_year);
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
        ESP_LOGI(TAG,"Loading month %d of year %d",cur_month,cur_year);

        // 2. Build the 42 lightweight cells
        appc_build_lightweight_calendar(ui_CalenderHolder, cur_month_offset, cur_month_days);

        // 3. Update ring visibilities based on loaded SD data
        for (int day = 1; day <= 31; day++) {
            day_task_status_t status = current_month_tasks[day - 1];
            appc_set_day_tasks(ui_CalenderHolder, day, cur_month_offset, status.task_one, status.task_two, status.task_three);
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

// Centralized function to rebuild calendar UI for cur_year & cur_month
void appc_render_calendar(void) {

    ESP_LOGI(TAG,"Rendering calender...");

    if (!ui_CalenderHolder) return;

    // 1. Recalculate offset and total days for the target month
    cur_month_days = get_days_in_month(cur_year, cur_month);
    cur_month_offset = get_month_start_offset(cur_year, cur_month);

    // 2. Read history from SD Card for target month
    load_month_from_sd(cur_year, cur_month);

    // 3. Rebuild calendar cells and update Month/Year labels
    appc_build_lightweight_calendar(ui_CalenderHolder, cur_month_offset, cur_month_days);

    // 4. Update ring visibilities using cur_month_offset
    for (int day = 1; day <= cur_month_days; day++) {
        day_task_status_t status = current_month_tasks[day - 1];
        appc_set_day_tasks(ui_CalenderHolder, day, cur_month_offset, 
                           status.task_one, status.task_two, status.task_three);
    }
}

// Previous Month Action
void appc_task_cal_prev(lv_event_t * e) {

    cur_month--;
    if (cur_month < 1) {
        cur_month = 12;
        cur_year--;
    }

    ESP_LOGI(TAG,"Rendering previous month %d",cur_month);

    appc_render_calendar();

    ESP_LOGI(TAG,"Rendered previous month %d",cur_month);
}

// Next Month Action
void appc_task_cal_next(lv_event_t * e) {
    cur_month++;
    if (cur_month > 12) {
        cur_month = 1;
        cur_year++;
    }

    ESP_LOGI(TAG,"Rendering next month %d",cur_month);

    appc_render_calendar();

    ESP_LOGI(TAG,"Rendered next month %d",cur_month);
}

void appc_tasks_init(void) {
    // 1. Remove the knob (thumb indicator) style entirely
    lv_obj_remove_style(ui_ArcTask1, NULL, LV_PART_KNOB);
    lv_obj_remove_style(ui_ArcTask2, NULL, LV_PART_KNOB);
    lv_obj_remove_style(ui_ArcTask3, NULL, LV_PART_KNOB);

    // 2. Make the knob invisible as a failsafe against default theme drawing
    lv_obj_set_style_opa(ui_ArcTask1, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_opa(ui_ArcTask2, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_opa(ui_ArcTask3, LV_OPA_TRANSP, LV_PART_KNOB);

    // 3. Disable touch interaction so it acts purely as a display ring
    lv_obj_clear_flag(ui_ArcTask1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_ArcTask2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_ArcTask3, LV_OBJ_FLAG_CLICKABLE);

    // Optional: Start the progress from 12 o'clock (top) instead of 3 o'clock
    lv_arc_set_rotation(ui_ArcTask1, 270);
    lv_arc_set_rotation(ui_ArcTask2, 270);
    lv_arc_set_rotation(ui_ArcTask3, 270);

    appc_load_all_tasks();

    appc_setup_clock_task_labels();

    if (ui_TaskDropdown) {
        lv_obj_add_event_cb(ui_TaskDropdown, ui_TaskDropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (ui_TaskSetBtn2) {
        lv_obj_add_event_cb(ui_TaskSetBtn2, ui_SetButton_event_cb, LV_EVENT_CLICKED, NULL);
    }

    if(ui_TaskOnGoingBtn) {
        lv_obj_add_event_cb(ui_TaskOnGoingBtn, ui_TaskOnGoingBtn_event_cb, LV_EVENT_CLICKED, NULL);
    }
}


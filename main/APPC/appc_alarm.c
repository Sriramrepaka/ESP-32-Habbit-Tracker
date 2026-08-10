#include "appc_alarm.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>

static const char *TAG = "APPC_ALARM";

LV_IMG_DECLARE(alarm_clock);
static lv_obj_t *s_ring_panel = NULL;

static appc_alarm_t s_alarms[MAX_ALARMS];
static int s_alarm_count = 0;
static int s_editing_index = -1; // -1 = Create New, >=0 = Edit Existing

// Light struct for NVS storage (strips out LVGL widget pointers)
typedef struct {
    int hour;
    int minute;
    bool enabled;
} persistent_alarm_t;

// Forward declarations
static void create_alarm_component(int hour, int minute, bool enabled);
static void update_alarm_label(appc_alarm_t *alarm);
static void save_alarms_to_nvs(void);
static void load_alarms_from_nvs(void);

// --- NVS Storage Functions ---

static void save_alarms_to_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("alarm_store", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    persistent_alarm_t storage_array[MAX_ALARMS];
    for (int i = 0; i < s_alarm_count; i++) {
        storage_array[i].hour = s_alarms[i].hour;
        storage_array[i].minute = s_alarms[i].minute;
        storage_array[i].enabled = s_alarms[i].enabled;
    }

    nvs_set_i32(handle, "alarm_cnt", s_alarm_count);
    nvs_set_blob(handle, "alarm_data", storage_array, sizeof(persistent_alarm_t) * s_alarm_count);
    
    err = nvs_commit(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %d alarm(s) to NVS", s_alarm_count);
    }
    nvs_close(handle);
}

static void load_alarms_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("alarm_store", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No NVS alarm data found (first boot)");
        return;
    }

    int32_t count = 0;
    if (nvs_get_i32(handle, "alarm_cnt", &count) == ESP_OK && count > 0) {
        persistent_alarm_t storage_array[MAX_ALARMS];
        size_t required_size = sizeof(persistent_alarm_t) * count;

        if (nvs_get_blob(handle, "alarm_data", storage_array, &required_size) == ESP_OK) {
            // Restore default SquareLine component (Index 0)
            s_alarms[0].hour = storage_array[0].hour;
            s_alarms[0].minute = storage_array[0].minute;
            s_alarms[0].enabled = storage_array[0].enabled;
            update_alarm_label(&s_alarms[0]);

            if (s_alarms[0].switch_obj) {
                if (s_alarms[0].enabled) {
                    lv_obj_add_state(s_alarms[0].switch_obj, LV_STATE_CHECKED);
                } else {
                    lv_obj_clear_state(s_alarms[0].switch_obj, LV_STATE_CHECKED);
                }
            }

            s_alarm_count = 1;

            // Dynamically reconstruct dynamic alarm components (Index 1 to count-1)
            for (int i = 1; i < count; i++) {
                create_alarm_component(storage_array[i].hour, storage_array[i].minute, storage_array[i].enabled);
            }
            ESP_LOGI(TAG, "Restored %d alarm(s) from NVS", s_alarm_count);
        }
    }
    nvs_close(handle);
}

// --- LVGL Event Callbacks ---

static void appc_alarm_card_click_cb(lv_event_t * e) {
    lv_obj_clear_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_NewAlarmBtn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(target);

    if (idx < 0 || idx >= s_alarm_count) return;

    s_editing_index = idx;

    int h = s_alarms[idx].hour;
    int m = s_alarms[idx].minute;

    lv_roller_set_selected(ui_RollerHour, (uint16_t)(h - 1), LV_ANIM_OFF);
    lv_roller_set_selected(ui_RollerMin, (uint16_t)(m / 5), LV_ANIM_OFF);
}

static void appc_alarm_switch_cb(lv_event_t * e) {
    lv_obj_t *sw = lv_event_get_target(e);
    lv_obj_t *parent_card = lv_obj_get_parent(sw);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(parent_card);

    if (idx >= 0 && idx < s_alarm_count) {
        s_alarms[idx].enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Alarm %d toggled: %s", idx, s_alarms[idx].enabled ? "ON" : "OFF");
        
        save_alarms_to_nvs(); // Auto-save toggle state change
    }
}

static void appc_alarm_new_btn_cb(lv_event_t * e) {
    s_editing_index = -1;

    lv_obj_clear_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_NewAlarmBtn, LV_OBJ_FLAG_HIDDEN);

    lv_roller_set_selected(ui_RollerHour, 0, LV_ANIM_OFF);
    lv_roller_set_selected(ui_RollerMin, 0, LV_ANIM_OFF);
}

static void appc_alarm_set_btn_cb(lv_event_t * e) {
    int sel_hour = (int)lv_roller_get_selected(ui_RollerHour) + 1;
    int sel_min  = (int)lv_roller_get_selected(ui_RollerMin) * 5;

    if (s_editing_index >= 0) {
        s_alarms[s_editing_index].hour = sel_hour;
        s_alarms[s_editing_index].minute = sel_min;
        update_alarm_label(&s_alarms[s_editing_index]);
    } else {
        create_alarm_component(sel_hour, sel_min, true);
    }

    save_alarms_to_nvs(); // Save new/updated alarm state to NVS

    lv_obj_add_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_NewAlarmBtn, LV_OBJ_FLAG_HIDDEN);
}

static void appc_alarm_del_btn_cb(lv_event_t * e) {
    // Check if we are editing an existing alarm
    if (s_editing_index >= 0 && s_editing_index < s_alarm_count) {
        int idx = s_editing_index;

        // 1. Destroy LVGL container widget (automatically deletes label & switch)
        if (s_alarms[idx].comp_obj) {
            lv_obj_del(s_alarms[idx].comp_obj);
        }

        // 2. Shift array elements left to fill the gap
        for (int i = idx; i < s_alarm_count - 1; i++) {
            s_alarms[i] = s_alarms[i + 1];
            
            // Critical: Update LVGL user_data tag to match the new array index
            if (s_alarms[i].comp_obj) {
                lv_obj_set_user_data(s_alarms[i].comp_obj, (void*)(uintptr_t)i);
            }
        }

        // 3. Decrement count and clear the last array slot
        s_alarm_count--;
        memset(&s_alarms[s_alarm_count], 0, sizeof(appc_alarm_t));

        // 4. Save updated array state to NVS flash
        save_alarms_to_nvs();
    }

    // 5. Reset state and hide modal
    s_editing_index = -1;
    lv_obj_add_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_NewAlarmBtn, LV_OBJ_FLAG_HIDDEN);
}

static void appc_alarm_stop_btn_cb(lv_event_t * e) {
    if (s_ring_panel) {
        lv_obj_add_flag(s_ring_panel, LV_OBJ_FLAG_HIDDEN); // Hide screen
    }
    // TODO: Silence hardware buzzer/audio PWM here
    Music_pause();
}

static void create_alarm_ring_screen(void) {
    // 1. Overlay Panel (240x320, 20% Opacity)
    s_ring_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_ring_panel, 240, 320);
    lv_obj_center(s_ring_panel);
    
    lv_obj_set_style_bg_color(s_ring_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ring_panel, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ring_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ring_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Static Image Widget
    lv_obj_t *img = lv_img_create(s_ring_panel);
    lv_img_set_src(img, &alarm_clock);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -30);

    // 3. STOP Button
    lv_obj_t *stop_btn = lv_btn_create(s_ring_panel);
    lv_obj_set_size(stop_btn, 100, 42);
    lv_obj_align(stop_btn, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0xD32F2F), LV_PART_MAIN);
    lv_obj_add_event_cb(stop_btn, appc_alarm_stop_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(stop_btn);
    lv_label_set_text(btn_label, "STOP");
    lv_obj_center(btn_label);

    // Hide by default
    lv_obj_add_flag(s_ring_panel, LV_OBJ_FLAG_HIDDEN);
}

void appc_alarm_check_time(int current_hour, int current_min, int current_sec) {
    // Prevent multiple triggers within the same minute
    static int s_last_triggered_min = -1;

    if (current_sec != 0) {
        s_last_triggered_min = -1; // Reset tracker when second moves past :00
        return;
    }

    if (current_min == s_last_triggered_min) {
        return; // Already evaluated this minute
    }

    s_last_triggered_min = current_min;

    for (int i = 0; i < s_alarm_count; i++) {
        if (!s_alarms[i].enabled) continue;

        // Normalize 24-hour roller value (24 -> 0 for midnight check)
        int alarm_hour = s_alarms[i].hour % 24;

        if (alarm_hour == current_hour && s_alarms[i].minute == current_min) {
            ESP_LOGI(TAG, "🔔 ALARM TRIGGERED! Alarm #%d (%02d:%02d)", i, current_hour, current_min);

            // TODO: Add your hardware action or UI ring popup here:
            // 1. Turn on PWM Buzzer / Audio DAC
            // 2. Clear hidden flag on an active alarm modal (e.g. ui_AlarmRingPanel)
            if (s_ring_panel) {
                lv_obj_clear_flag(s_ring_panel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_ring_panel);
                Play_Music("/spiffs", "bootup.mp3");
            }
        }
    }
}

// --- Module Initialization ---

void appc_alarm_init(void) {
    if (ui_Alarm_container) {
        lv_obj_set_flex_flow(ui_Alarm_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ui_Alarm_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ui_Alarm_container, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_add_flag(ui_Alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(ui_Alarm_container, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(ui_Alarm_container, LV_SCROLLBAR_MODE_AUTO);
    }

    if (ui_RollerHour) {
        lv_obj_set_style_text_font(ui_RollerHour, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_roller_set_options(ui_RollerHour, 
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24", 
            LV_ROLLER_MODE_NORMAL);
    }

    if (ui_RollerMin) {
        lv_obj_set_style_text_font(ui_RollerMin, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_roller_set_options(ui_RollerMin, 
            "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55", 
            LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(ui_RollerMin, 3);
    }

    // Bind base card UI created by SquareLine Studio
    if (ui_AlarmComp && s_alarm_count < MAX_ALARMS) {
        s_alarms[0].hour = 0;
        s_alarms[0].minute = 0;
        s_alarms[0].enabled = true;
        s_alarms[0].comp_obj = ui_AlarmComp;
        s_alarms[0].label_obj = ui_Alarm_Num1;
        s_alarms[0].switch_obj = ui_Switch2;

        update_alarm_label(&s_alarms[0]);

        lv_obj_set_user_data(ui_AlarmComp, (void*)(uintptr_t)0);
        lv_obj_add_event_cb(ui_AlarmComp, appc_alarm_card_click_cb, LV_EVENT_LONG_PRESSED, NULL);

        if (ui_Switch2) {
            lv_obj_add_event_cb(ui_Switch2, appc_alarm_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        s_alarm_count = 1;
    }

    if (ui_NewAlarmBtn) {
        lv_obj_add_event_cb(ui_NewAlarmBtn, appc_alarm_new_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    if (ui_AlarmSetBtn) {
        lv_obj_add_event_cb(ui_AlarmSetBtn, appc_alarm_set_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    if (ui_AlarmDelBtn) {
        lv_obj_add_event_cb(ui_AlarmDelBtn, appc_alarm_del_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    create_alarm_ring_screen();

    // Restore saved alarms on boot
    load_alarms_from_nvs();
}

// --- Dynamic Component Creation ---

static void update_alarm_label(appc_alarm_t *alarm) {
    if (alarm && alarm->label_obj) {
        lv_label_set_text_fmt(alarm->label_obj, "%02d:%02d", alarm->hour, alarm->minute);
    }
}

static void create_alarm_component(int hour, int minute, bool enabled) {
    if (s_alarm_count >= MAX_ALARMS) {
        ESP_LOGW(TAG, "Max alarm count reached!");
        return;
    }

    int idx = s_alarm_count;

    lv_obj_t *comp = lv_obj_create(ui_Alarm_container);
    lv_obj_set_height(comp, 80);
    lv_obj_set_width(comp, lv_pct(94));
    lv_obj_clear_flag(comp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(comp, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(comp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(comp, lv_color_hex(0x8A3200), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(comp, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(comp, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(comp, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(comp, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(comp, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(comp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(comp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_user_data(comp, (void*)(uintptr_t)idx);
    lv_obj_add_event_cb(comp, appc_alarm_card_click_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *lbl = lv_label_create(comp);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_obj_set_align(lbl, LV_ALIGN_LEFT_MID);
    ui_object_set_themeable_style_property(lbl, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR, _ui_theme_color_Neon_orange);
    ui_object_set_themeable_style_property(lbl, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA, _ui_theme_alpha_Neon_orange);
    lv_obj_set_style_text_font(lbl, &ui_font_Number, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *sw = lv_switch_create(comp);
    lv_obj_set_width(sw, 61);
    lv_obj_set_height(sw, 32);
    lv_obj_set_x(sw, -4);
    lv_obj_set_y(sw, 11);
    lv_obj_set_align(sw, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_radius(sw, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x9D9ED5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sw, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(sw, 50, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x293062), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, 255, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 50, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sw, 255, LV_PART_KNOB | LV_STATE_DEFAULT);

    if (enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, appc_alarm_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_alarms[idx].hour = hour;
    s_alarms[idx].minute = minute;
    s_alarms[idx].enabled = enabled;
    s_alarms[idx].comp_obj = comp;
    s_alarms[idx].label_obj = lbl;
    s_alarms[idx].switch_obj = sw;

    update_alarm_label(&s_alarms[idx]);
    s_alarm_count++;
}
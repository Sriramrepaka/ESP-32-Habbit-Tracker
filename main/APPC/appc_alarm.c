#include "appc_alarm.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "APPC_ALARM";

static appc_alarm_t s_alarms[MAX_ALARMS];
static int s_alarm_count = 0;
static int s_editing_index = -1; // -1 = Create New, >=0 = Edit Existing

// Forward declarations for internal helpers
static void create_alarm_component(int hour, int minute, bool enabled);
static void update_alarm_label(appc_alarm_t *alarm);

// --- LVGL Event Callbacks ---

static void appc_alarm_card_click_cb(lv_event_t * e) {

    lv_obj_clear_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_NewAlarmBtn,LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(target);

    if (idx < 0 || idx >= s_alarm_count) return;

    s_editing_index = idx;

    // Set rollers to target alarm's values
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
    }
}

static void appc_alarm_new_btn_cb(lv_event_t * e) {

    s_editing_index = -1; // Set mode to NEW

    lv_obj_clear_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_NewAlarmBtn,LV_OBJ_FLAG_HIDDEN);

    // Default roller values (07:00)
    lv_roller_set_selected(ui_RollerHour, 0, LV_ANIM_OFF); // Index 6 = "7"
    lv_roller_set_selected(ui_RollerMin, 0, LV_ANIM_OFF);  // Index 0 = "00"

}

static void appc_alarm_set_btn_cb(lv_event_t * e) {

    int sel_hour = (int)lv_roller_get_selected(ui_RollerHour) + 1;
    int sel_min  = (int)lv_roller_get_selected(ui_RollerMin) * 5;
    ESP_LOGI(TAG,"Display hour: %d Selected hour: %d, Display min: %d Selected min: %d",(int)lv_roller_get_selected(ui_RollerHour),sel_hour,(int)lv_roller_get_selected(ui_RollerMin),sel_min);

    if (s_editing_index >= 0) {
        // Edit existing alarm
        s_alarms[s_editing_index].hour = sel_hour;
        s_alarms[s_editing_index].minute = sel_min;
        update_alarm_label(&s_alarms[s_editing_index]);
    } else {
        // Create new alarm card
        create_alarm_component(sel_hour, sel_min, true);
    }

    // Hide configuration modal
    lv_obj_add_flag(ui_AlarmSetPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_NewAlarmBtn,LV_OBJ_FLAG_HIDDEN);
}

// --- Module Initialization ---

void appc_alarm_init(void) {
    // 1. Convert container to Flexbox column layout
    if (ui_Alarm_container) {
        lv_obj_set_flex_flow(ui_Alarm_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ui_Alarm_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ui_Alarm_container, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_add_flag(ui_Alarm_container, LV_OBJ_FLAG_SCROLLABLE);            // Allow touch dragging/scrolling
        lv_obj_set_scroll_dir(ui_Alarm_container, LV_DIR_VER);                 // Restrict scrolling to Y-axis
        lv_obj_set_scrollbar_mode(ui_Alarm_container, LV_SCROLLBAR_MODE_AUTO); // Show scrollbar only when scrolling
    }

    // 2. Configure Rollers (Fixes visual index offset & scrolling alignment)
    if (ui_RollerHour) {
        lv_obj_set_style_text_font(ui_RollerHour, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_roller_set_options(ui_RollerHour, 
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24", 
            LV_ROLLER_MODE_NORMAL);
        ///lv_roller_set_visible_row_count(ui_RollerHour, 3);
    }

    if (ui_RollerMin) {
        lv_obj_set_style_text_font(ui_RollerMin, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_roller_set_options(ui_RollerMin, 
            "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55", 
            LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(ui_RollerMin, 3);
    }

    // 3. Register default card created by SquareLine Studio
    if (ui_AlarmComp && s_alarm_count < MAX_ALARMS) {
        s_alarms[0].hour = 0;
        s_alarms[0].minute = 0;
        s_alarms[0].enabled = true;
        s_alarms[0].comp_obj = ui_AlarmComp;
        s_alarms[0].label_obj = ui_Alarm_Num1;
        s_alarms[0].switch_obj = ui_Switch2;

        update_alarm_label(&s_alarms[0]);

        lv_obj_set_user_data(ui_AlarmComp, (void*)(uintptr_t)0);
        lv_obj_add_event_cb(ui_AlarmComp, appc_alarm_card_click_cb, LV_EVENT_CLICKED, NULL);

        if (ui_Switch2) {
            lv_obj_add_event_cb(ui_Switch2, appc_alarm_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        s_alarm_count = 1;
    }

    // 4. Attach event callbacks to action buttons
    if (ui_NewAlarmBtn) {
        lv_obj_add_event_cb(ui_NewAlarmBtn, appc_alarm_new_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    if (ui_AlarmSetBtn) {
        lv_obj_add_event_cb(ui_AlarmSetBtn, appc_alarm_set_btn_cb, LV_EVENT_CLICKED, NULL);
    }
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

    // Card Panel
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

    // Store index tag & attach click callback
    lv_obj_set_user_data(comp, (void*)(uintptr_t)idx);
    lv_obj_add_event_cb(comp, appc_alarm_card_click_cb, LV_EVENT_CLICKED, NULL);

    // Time Label
    lv_obj_t *lbl = lv_label_create(comp);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_obj_set_align(lbl, LV_ALIGN_LEFT_MID);
    ui_object_set_themeable_style_property(lbl, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR, _ui_theme_color_Neon_orange);
    ui_object_set_themeable_style_property(lbl, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA, _ui_theme_alpha_Neon_orange);
    lv_obj_set_style_text_font(lbl, &ui_font_Number, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Switch
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

    // Save instance properties
    s_alarms[idx].hour = hour;
    s_alarms[idx].minute = minute;
    s_alarms[idx].enabled = enabled;
    s_alarms[idx].comp_obj = comp;
    s_alarms[idx].label_obj = lbl;
    s_alarms[idx].switch_obj = sw;

    update_alarm_label(&s_alarms[idx]);
    s_alarm_count++;
}
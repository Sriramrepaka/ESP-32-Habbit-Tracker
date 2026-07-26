#include "appc_sketch.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

static const char *TAG = "APPC_SKETCH";

// Perfect 32-bit CPU-word stepping multiples matching your updated SquareLine project size
#define CANVAS_WIDTH  224
#define CANVAS_HEIGHT 288

static uint8_t *canvas_buffer = NULL;
static lv_obj_t *canvas = NULL;
static lv_point_t last_point = {-1, -1};

static lv_obj_t *view_canvas = NULL;
static uint8_t *view_canvas_buffer = NULL;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void note_button_click_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (!label) return;
        const char *note_name = lv_label_get_text(label);
        
        char full_path[64];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bin", note_name);
        ESP_LOGI(TAG, "Loading saved true-color note from: %s", full_path);

        FILE *f = fopen(full_path, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open note binary file.");
            return;
        }
        
        size_t true_color_size = CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(lv_color_t);
        if (!view_canvas_buffer) {
            view_canvas_buffer = heap_caps_malloc(true_color_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!view_canvas_buffer) {
                ESP_LOGE(TAG, "Failed to allocate view canvas buffer");
                fclose(f);
                return;
            }
        }
        
        size_t read_bytes = fread(view_canvas_buffer, 1, true_color_size, f);
        fclose(f);

        if (read_bytes != true_color_size) {
            ESP_LOGE(TAG, "Read mismatch. Expected %zu bytes, got %zu.", true_color_size, read_bytes);
            return;
        }

        if (!view_canvas) {
            view_canvas = lv_canvas_create(ui_SketchViewPanel);
            lv_obj_center(view_canvas);
        }
        
        lv_canvas_set_buffer(view_canvas, view_canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(view_canvas); 

        lv_obj_clear_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
    }
}

void appc_notes_list_populate(lv_event_t * e) {
    lv_obj_clean(ui_NotesDisplayPanel);
    lv_obj_set_flex_flow(ui_NotesDisplayPanel, LV_FLEX_FLOW_COLUMN); 

    DIR *dir = opendir("/sdcard/");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open /sdcard root directory.");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *ext = strrchr(ent->d_name, '.');
        if (ext && strcmp(ext, ".bin") == 0) {
            char clean_name[32];
            size_t len = ext - ent->d_name;
            if (len >= sizeof(clean_name)) len = sizeof(clean_name) - 1;
            strncpy(clean_name, ent->d_name, len);
            clean_name[len] = '\0';

            lv_obj_t *new_btn = lv_btn_create(ui_NotesDisplayPanel);
            lv_obj_set_height(new_btn, 40);
            lv_obj_set_width(new_btn, lv_pct(100));
            
            lv_obj_add_flag(new_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_clear_flag(new_btn, LV_OBJ_FLAG_SCROLLABLE);
            
            lv_obj_set_style_bg_color(new_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(new_btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_side(new_btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_t *label = lv_label_create(new_btn);
            if (label != NULL) {
                lv_obj_set_width(label, LV_SIZE_CONTENT);
                lv_obj_set_height(label, LV_SIZE_CONTENT);
                lv_obj_set_align(label, LV_ALIGN_CENTER);
                lv_label_set_text(label, clean_name); 
                lv_obj_set_style_text_color(label, lv_color_hex(0x120000), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            }

            lv_obj_add_event_cb(new_btn, note_button_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    closedir(dir);
}

void appc_notes_view_close(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
        
        if (view_canvas) {
            lv_obj_del(view_canvas);
            view_canvas = NULL;
        }
        if (view_canvas_buffer) {
            heap_caps_free(view_canvas_buffer);
            view_canvas_buffer = NULL;
        }
    }
}

static void sketch_canvas_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSING && code != LV_EVENT_PRESSED) return;
    
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t curr_point;
    lv_indev_get_point(indev, &curr_point);
    
    lv_area_t rect;
    lv_obj_get_coords(canvas, &rect);
    
    // Log BEFORE subtraction so we see raw vs canvas-local
    ESP_LOGI(TAG, "raw=(%d,%d) canvas_origin=(%d,%d) local=(%d,%d)",
        curr_point.x, curr_point.y,
        rect.x1, rect.y1,
        curr_point.x - rect.x1,
        curr_point.y - rect.y1);

    curr_point.x -= rect.x1;
    curr_point.y -= rect.y1;

    curr_point.x = LV_CLAMP(0, curr_point.x, CANVAS_WIDTH - 1);
    curr_point.y = LV_CLAMP(0, curr_point.y, CANVAS_HEIGHT - 1);

    if (code == LV_EVENT_PRESSED) {
        last_point = curr_point;
    } 
    else if (code == LV_EVENT_PRESSING) {
        if (last_point.x == -1 || last_point.y == -1) {
            last_point = curr_point;
            return; 
        }

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_black(); 
        line_dsc.width = 3;                 
        line_dsc.round_start = 1;
        line_dsc.round_end = 1;

        lv_point_t points[2] = { last_point, curr_point };
        lv_canvas_draw_line(canvas, points, 2, &line_dsc);
        
        last_point = curr_point;
        
        // Ensure changes are pushed immediately to the display pipeline
        lv_obj_invalidate(canvas);
    } 
    else if (code == LV_EVENT_RELEASED) {
        last_point.x = -1;
        last_point.y = -1;
    }
}

void appc_sketch_clear(void) {
    if (canvas && canvas_buffer) {
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
    }
}

void appc_sketch_close(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        appc_sketch_clear();
        last_point.x = -1;
        last_point.y = -1;
    }
}

void appc_sketch_save(lv_event_t * e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    }
}

void appc_note_save(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        const char *filename = lv_textarea_get_text(ui_TextArea3);
        if (filename == NULL || strlen(filename) == 0) {
            ESP_LOGW(TAG, "Cannot save note: Filename string is empty!");
            return;
        }

        char full_path[64];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bin", filename);
        ESP_LOGI(TAG, "Saving true-color sketch to path: %s", full_path);

        if (canvas_buffer == NULL) return;

        FILE *f = fopen(full_path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to create file.");
            lv_textarea_set_text(ui_TextArea3, "");
            return;
        }

        size_t true_color_size = CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(lv_color_t);
        size_t written = fwrite(canvas_buffer, 1, true_color_size, f);
        fclose(f);

        if (written == true_color_size) {
            ESP_LOGI(TAG, "True-color file saved successfully.");
            
            _ui_flag_modify(ui_NotesNamePanel, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
            _ui_screen_change(&ui_Clock, LV_SCR_LOAD_ANIM_MOVE_TOP, 500, 0, &ui_Clock_screen_init);

            lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
            lv_textarea_set_text(ui_TextArea3, "");
            
            appc_sketch_clear();
            last_point.x = -1;
            last_point.y = -1;
        } else {
            ESP_LOGE(TAG, "Write capacity error.");
        }
    }
}

void appc_note_cancel(lv_event_t * e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_textarea_set_text(ui_TextArea3, "");
        appc_sketch_clear();
        last_point.x = -1;
        last_point.y = -1;
    }
}

static void force_redraw_cb(lv_timer_t *timer) {
    lv_obj_t *cv = (lv_obj_t *)timer->user_data;
    lv_canvas_fill_bg(cv, lv_color_white(), LV_OPA_COVER);
    lv_obj_invalidate(cv);
    lv_timer_del(timer);
}

void appc_sketch_init(void) {
    last_point.x = -1;
    last_point.y = -1;
    
    if (canvas_buffer != NULL) {
        if (canvas) {
            lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);  // re-assert position
            lv_obj_invalidate(canvas);
        }
        return;
    }

    if (ui_Panel4 == NULL) return;

    size_t buffer_size = LV_IMG_BUF_SIZE_TRUE_COLOR(CANVAS_WIDTH, CANVAS_HEIGHT);
    canvas_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (canvas_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate true-color sketch buffer!");
        return;
    }

    memset(canvas_buffer, 0xFF, buffer_size);

    // 1. ANNIHILATE ALL PARENT PADDING, BORDERS, AND SCROLLING
    lv_obj_set_style_pad_all(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Panel4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_Panel4, LV_OBJ_FLAG_GESTURE_BUBBLE); // Stops swipes from shifting coordinates
    lv_obj_set_scrollbar_mode(ui_Panel4, LV_SCROLLBAR_MODE_OFF);

    lv_obj_update_layout(ui_Panel4);

    canvas = lv_canvas_create(ui_Panel4);
    if (canvas == NULL) return;

    // 2. STRIP CANVAS STYLES TO PREVENT INHERITED OFFSETS
    lv_obj_set_style_pad_all(canvas, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(canvas, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    //lv_obj_set_style_margin_all(canvas, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 3. DISABLE CANVAS SCROLLING AND GESTURE BUBBLING
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_GESTURE_BUBBLE); 

    lv_canvas_set_buffer(canvas, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

    // Diagnostic: thick red bar at the very top, thick blue bar at the very bottom
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = lv_color_make(255, 0, 0);
    rect_dsc.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_WIDTH, 30, &rect_dsc);   // 30px thick now

    rect_dsc.bg_color = lv_color_make(0, 255, 0);
    lv_canvas_draw_rect(canvas, 0, 30, CANVAS_WIDTH, 30, &rect_dsc);  // green band rows 30-59

    rect_dsc.bg_color = lv_color_make(0, 0, 255);
    lv_canvas_draw_rect(canvas, 0, CANVAS_HEIGHT - 10, CANVAS_WIDTH, 10, &rect_dsc);

        
    lv_obj_invalidate(canvas);

    lv_obj_set_size(canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    
    // 4. CRITICAL: ANCHOR TOP-LEFT INSTEAD OF CENTERING
    // This absolutely guarantees the drawing math starts at (0,0) with no negative offsets
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_update_layout(canvas);
    lv_obj_add_event_cb(canvas, sketch_canvas_event_cb, LV_EVENT_ALL, NULL);

    ESP_LOGI(TAG, "Canvas coords: x1=%d y1=%d", lv_obj_get_x(canvas), lv_obj_get_y(canvas));
    
     lv_obj_invalidate(ui_Panel4);
    lv_obj_invalidate(canvas);

    lv_timer_create(force_redraw_cb, 200, canvas);

    lv_obj_t *test_label = lv_label_create(ui_Panel4);
    lv_label_set_text(test_label, "ROW0");
    lv_obj_align(test_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(test_label, lv_color_make(255, 0, 255), 0);  // magenta
    lv_obj_set_style_bg_color(test_label, lv_color_make(255, 255, 0), 0);    // yellow background
    lv_obj_set_style_bg_opa(test_label, LV_OPA_COVER, 0);                    // make bg visible
    ESP_LOGI(TAG, "True-Color Canvas Locked and Initialized.");
}

void appc_sketch_deinit(void) {
    if (canvas) {
        lv_obj_del(canvas);
        canvas = NULL;
    }
    if (canvas_buffer) {
        heap_caps_free(canvas_buffer);
        canvas_buffer = NULL;
    }
}
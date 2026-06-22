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

#define CANVAS_WIDTH  218
#define CANVAS_HEIGHT 291

#define MAX_NOTES 20

static uint8_t *canvas_buffer = NULL;
static lv_obj_t *canvas = NULL;
static lv_point_t last_point = {0, 0};

static lv_obj_t *view_canvas = NULL;
static uint8_t *view_canvas_buffer = NULL;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void note_button_click_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (code == LV_EVENT_CLICKED) {
        // 1. Recover the text label from inside this button
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (!label) return;
        const char *note_name = lv_label_get_text(label);
        
        char full_path[64];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bin", note_name);
        ESP_LOGI(TAG, "Loading saved note from: %s", full_path);

        // 2. Open file and read binary contents
        FILE *f = fopen(full_path, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open note binary file.");
            return;
        }

        size_t buffer_size = CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(lv_color_t);
        
        // Allocate preview canvas buffer in PSRAM if it doesn't exist
        if (!view_canvas_buffer) {
            view_canvas_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        
        size_t read_bytes = fread(view_canvas_buffer, 1, buffer_size, f);
        fclose(f);

        if (read_bytes != buffer_size) {
            ESP_LOGE(TAG, "Read mismatch. File may be corrupted.");
            return;
        }

        // 3. Mount view canvas onto SquareLine's ui_SketchViewPanel
        if (!view_canvas) {
            view_canvas = lv_canvas_create(ui_SketchViewPanel);
            lv_obj_center(view_canvas);
        }
        
        lv_canvas_set_buffer(view_canvas, view_canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(view_canvas); // Force flush refresh data to panel

        // 4. Show the View Panel container
        lv_obj_clear_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
    }
}

// Scans the storage directory and populates ui_NotesDisplayPanel with buttons
void appc_notes_list_populate(lv_event_t * e) {

    // 1. Reset existing items inside the container to avoid duplication stacking
    lv_obj_clean(ui_NotesDisplayPanel);
    lv_obj_set_flex_flow(ui_NotesDisplayPanel, LV_FLEX_FLOW_COLUMN); // Arrange vertically automatically

    DIR *dir =  opendir("/sdcard/");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open /sdcard root directory.");
        return;
    }

    struct dirent *ent;
    // 2. Loop through all existing files in storage
    while ((ent = readdir(dir)) != NULL) {
        // Filter out files that don't match our .bin criteria extension
        char *ext = strrchr(ent->d_name, '.');
        if (ext && strcmp(ext, ".bin") == 0) {
            
            // Isolate the filename without the extension for clean displaying
            char clean_name[32];
            size_t len = ext - ent->d_name;
            if (len >= sizeof(clean_name)) len = sizeof(clean_name) - 1;
            strncpy(clean_name, ent->d_name, len);
            clean_name[len] = '\0';

            // Create new button following your requested formatting layout rules
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

            // Create text element label injection
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

            // Assign unified listener logic
            lv_obj_add_event_cb(new_btn, note_button_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    closedir(dir);
}

void appc_notes_view_close(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
        
        // Optional memory clean up for preview buffer when panel is hidden
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

// Touch event tracking
static void sketch_canvas_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &last_point);
        lv_area_t rect;
        lv_obj_get_coords(canvas, &rect);
        last_point.x -= rect.x1;
        last_point.y -= rect.y1;
        
        // FIX: Reject initial point instantly if it's outside boundaries
        if (last_point.x < 0 || last_point.x >= CANVAS_WIDTH || 
            last_point.y < 0 || last_point.y >= CANVAS_HEIGHT) {
            last_point.x = -1;
            last_point.y = -1;
        }
    } 
    else if (code == LV_EVENT_PRESSING) {
        lv_point_t current_point;
        lv_indev_get_point(indev, &current_point);
        
        lv_area_t rect;
        lv_obj_get_coords(canvas, &rect);
        current_point.x -= rect.x1;
        current_point.y -= rect.y1;

        // FIX: Enforce a strict 2-pixel safety margin padding inside the canvas edges
        // This stops your finger from trailing over the container borders and generating noise.
        const int padding = 2; 
        if (current_point.x >= padding && current_point.x < (CANVAS_WIDTH - padding) &&
            current_point.y >= padding && current_point.y < (CANVAS_HEIGHT - padding) &&
            last_point.x >= padding && last_point.x < (CANVAS_WIDTH - padding) &&
            last_point.y >= padding && last_point.y < (CANVAS_HEIGHT - padding)) {
            
            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = lv_color_black(); 
            line_dsc.width = 3;                 
            line_dsc.round_start = 1;
            line_dsc.round_end = 1;

            lv_point_t points[2] = {last_point, current_point};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            
            // Only update the path tracker if we successfully drew a valid internal segment
            last_point = current_point;
        } else {
            // Discard path reference if the brush slips out of bounds
            last_point.x = -1;
            last_point.y = -1;
        }
    } 
    else if (code == LV_EVENT_RELEASED) {
        last_point.x = -1;
        last_point.y = -1;
    }
}

void appc_sketch_clear(void) {
    if (canvas) {
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
    }
}

void appc_sketch_close(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    // Only execute if the target object was clicked/released
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Exiting sketchpad: discarding unsaved modifications.");
        
        // 1. Wipe the panel surface completely back to black
        appc_sketch_clear();
        
        // 2. Clear out touch tracking memory safety boundaries
        last_point.x = -1;
        last_point.y = -1;
    }
}

void appc_sketch_save(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Save button clicked. Displaying filename entry panel.");
        // Keep memory alive during file configuration keyboard interactions
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        //lv_obj_clear_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN); 
    }
}

void appc_note_save(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        // Fetch the raw string typed inside SquareLine's TextArea3
        const char *filename = lv_textarea_get_text(ui_TextArea3);
        
        // Safety validation if text area is left empty
        if (filename == NULL || strlen(filename) == 0) {
            ESP_LOGW(TAG, "Cannot save note: Filename string is empty!");
            return;
        }

        // Construct full storage filepath directory path string
        
        char full_path[64];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bin", filename);
        
        ESP_LOGI(TAG, "Attempting to save note to path: %s", full_path);

        if (canvas_buffer == NULL) {
            ESP_LOGE(TAG, "Canvas data buffer is empty or unallocated.");
            return;
        }

        // Standard ESP-IDF virtual filesystem file creation
        FILE *f = fopen(full_path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to create file inside storage filesystem partition.");
            lv_textarea_set_text(ui_TextArea3, "");
            return;
        }

        size_t buffer_size = CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(lv_color_t);
        size_t written = fwrite(canvas_buffer, 1, buffer_size, f);
        fclose(f);

        if (written == buffer_size) {
            ESP_LOGI(TAG, "Note successfully written onto flash layout!");
            
            // Re-hide the save dialog box overlay automatically upon successful export
            _ui_flag_modify(ui_NotesNamePanel, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
            _ui_screen_change(&ui_Clock, LV_SCR_LOAD_ANIM_MOVE_TOP, 500, 0, &ui_Clock_screen_init);

            lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
            lv_textarea_set_text(ui_TextArea3, "");
            
            // Clear the workspace and reset tracking states
            appc_sketch_clear();
            last_point.x = -1;
            last_point.y = -1;
        } else {
            ESP_LOGE(TAG, "File system capacity write mismatch failure error.");
        }
    }
}

void appc_note_cancel(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {

        // lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        // lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        // lv_obj_add_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_textarea_set_text(ui_TextArea3, "");

        appc_sketch_clear();
        last_point.x = -1;
        last_point.y = -1;
    }
}

// Dynamically Allocates PSRAM and creates canvas when screen opens
void appc_sketch_init(void) {

    if (canvas_buffer != NULL) {
        ESP_LOGW(TAG, "Sketchpad already initialized.");
        if (canvas) {
            lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        }
        return;
    }

    // Allocate 16-bit RGB565 Buffer strictly inside external PSRAM
    size_t buffer_size = CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(lv_color_t);
    canvas_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (canvas_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sketch buffer in PSRAM!");
        return;
    }

    // Attach Canvas into ui_Panel4 viewport
    canvas = lv_canvas_create(ui_Panel4);
    lv_canvas_set_buffer(canvas, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_obj_center(canvas);

    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_Panel4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas, sketch_canvas_event_cb, LV_EVENT_ALL, NULL);
    
    ESP_LOGI(TAG, "Dynamic Sketchpad allocated in PSRAM.");
}

// Safely destroys canvas components to completely free up memory 
void appc_sketch_deinit(void) {
    if (canvas) {
        lv_obj_del(canvas);
        canvas = NULL;
    }
    if (canvas_buffer) {
        heap_caps_free(canvas_buffer);
        canvas_buffer = NULL;
        ESP_LOGI(TAG, "Sketchpad PSRAM freed.");
    }
}
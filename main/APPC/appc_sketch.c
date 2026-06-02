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

#define CANVAS_STRIDE ((CANVAS_WIDTH + 7) / 8)
#define CANVAS_BYTE_SIZE (CANVAS_STRIDE * CANVAS_HEIGHT + 64)
#define MAX_NOTES 20

// Task management flags
static TaskHandle_t xNotesLoaderTaskHandle = NULL;
static volatile bool notes_data_ready = false; // Thread-safe signaling flag

// Shared array to hold names fetched by the background worker
static char fetched_note_names[MAX_NOTES][256];
static int fetched_note_count = 0;

static lv_obj_t *note_buttons[MAX_NOTES];
static int note_count = 0;

static char current_editing_file[128] = {0};
static char selected_note_path[128] = {0};

// Convert static arrays to pointers so they can be allocated from PSRAM
static uint8_t *canvas_buffer = NULL;
static lv_obj_t *canvas_obj = NULL;
static lv_point_t last_point = {0, 0};

static uint8_t *viewer_canvas_buffer = NULL;
static lv_obj_t *viewer_canvas_obj = NULL;
static lv_point_t viewer_last_point = {0, 0};

// Forward declarations of our async callbacks
static void appc_sketch_async_row_painter(void * param);
static void notes_loader_worker_task(void *pvParameters);

static void sketch_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();
    if(!indev) return;

    lv_point_t curr_point;
    lv_indev_get_point(indev, &curr_point);
    
    lv_area_t a;
    lv_obj_get_coords(canvas_obj, &a);
    curr_point.x -= a.x1;
    curr_point.y -= a.y1;

    if(code == LV_EVENT_PRESSED) {
        last_point = curr_point;
    } else if(code == LV_EVENT_PRESSING) {
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(0x000000);
        line_dsc.width = 2;        
        line_dsc.round_start = true; 
        line_dsc.round_end = true;
        
        lv_canvas_draw_line(canvas_obj, (lv_point_t[]){last_point, curr_point}, 2, &line_dsc);
        last_point = curr_point;
    }
}

static void viewer_sketch_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();
    if(!indev) return;

    lv_point_t curr_point;
    lv_indev_get_point(indev, &curr_point);
    
    lv_area_t a;
    lv_obj_get_coords(viewer_canvas_obj, &a);
    curr_point.x -= a.x1;
    curr_point.y -= a.y1;

    if(code == LV_EVENT_PRESSED) {
        viewer_last_point = curr_point;
    } else if(code == LV_EVENT_PRESSING) {
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(0x000000); 
        line_dsc.width = 2;        
        line_dsc.round_start = true; 
        line_dsc.round_end = true;
        
        lv_canvas_draw_line(viewer_canvas_obj, (lv_point_t[]){viewer_last_point, curr_point}, 2, &line_dsc);
        viewer_last_point = curr_point;
    }
}

void appc_sketch_clear(void){
    if (canvas_buffer != NULL) {
        memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
        lv_obj_invalidate(canvas_obj);
    }
}

void appc_sketch_close(lv_event_t * e){
    appc_sketch_clear();
}

void appc_sketch_set_visible(bool visible) {
    if (canvas_obj == NULL) return;

    if (visible) {
        lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(canvas_obj);
    } else {
        lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Replaces old trigger loop workflow. Safely calls FreeRTOS task creation.
void appc_sketch_trigger_refresh(void) {
    if (xNotesLoaderTaskHandle == NULL) {
        notes_data_ready = false;
        xTaskCreatePinnedToCore(
            notes_loader_worker_task, 
            "NotesWorker", 
            4096, 
            NULL, 
            2, 
            &xNotesLoaderTaskHandle, 
            1 // Core 1 (Keeps SD I/O completely away from Core 0)
        );
    }
}

void appc_sketch_init(void) {
    if (canvas_obj != NULL) {
        return; 
    }

    if (ui_Panel4 == NULL) {
        return; 
    }

    // Allocate Canvas Buffers dynamically out of PSRAM to reclaim internal heap room
    if (canvas_buffer == NULL) {
        canvas_buffer = heap_caps_malloc(CANVAS_BYTE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (canvas_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate main canvas buffer in PSRAM!");
            return;
        }
    }

    if (viewer_canvas_buffer == NULL) {
        viewer_canvas_buffer = heap_caps_malloc(CANVAS_BYTE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (viewer_canvas_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate viewer canvas buffer in PSRAM!");
            return;
        }
    }

    memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
    memset(viewer_canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);

    if (ui_SketchViewSaveBtn2 != NULL) lv_obj_set_ext_click_area(ui_SketchViewSaveBtn2, 20);
    if (ui_SketchViewCloseBtn2 != NULL) lv_obj_set_ext_click_area(ui_SketchViewCloseBtn2, 20);
    if (ui_SketchViewDeleteBtn1 != NULL) lv_obj_set_ext_click_area(ui_SketchViewDeleteBtn1, 20);

    lv_obj_t * parent = lv_obj_get_parent(ui_Panel4);
    canvas_obj = lv_canvas_create(parent);
    if(canvas_obj == NULL) return;

    lv_canvas_set_buffer(canvas_obj, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);

    lv_coord_t x_ofs = lv_obj_get_x_aligned(ui_Panel4);
    lv_coord_t y_ofs = lv_obj_get_y_aligned(ui_Panel4);
    lv_obj_align(canvas_obj, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_size(canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);

    if (ui_SketchViewPanel != NULL) {
        viewer_canvas_obj = lv_canvas_create(ui_SketchViewPanel);
        if (viewer_canvas_obj != NULL) {
            lv_canvas_set_buffer(viewer_canvas_obj, viewer_canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);
            lv_obj_set_size(viewer_canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);
            lv_obj_align(viewer_canvas_obj, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(ui_SketchViewPanel, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(ui_SketchViewPanel, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_flag(viewer_canvas_obj, LV_OBJ_FLAG_CLICKABLE);        
            lv_obj_clear_flag(viewer_canvas_obj, LV_OBJ_FLAG_SCROLLABLE);    
            lv_obj_add_event_cb(viewer_canvas_obj, viewer_sketch_event_cb, LV_EVENT_ALL, NULL); 
            lv_obj_move_background(viewer_canvas_obj); 
        }
    } else {
        ESP_LOGE(TAG, "ui_SketchViewPanel container object layout is missing!");
    }

    lv_obj_invalidate(canvas_obj);
    lv_obj_add_flag(ui_Panel4, LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(canvas_obj);

    lv_obj_add_event_cb(canvas_obj, sketch_event_cb, LV_EVENT_ALL, NULL);
    
    // Kickoff initial generation safely
    appc_sketch_trigger_refresh();
    ESP_LOGI(TAG, "Sketchpad Dynamic Init Successful");
}

void save_canvas_to_bmp(const char *filename) {
    if (canvas_buffer == NULL) return;
    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for writing!", filename);
        return;
    }

    uint32_t row_size = CANVAS_STRIDE; 
    uint32_t image_size = row_size * CANVAS_HEIGHT;
    uint32_t file_size = 54 + 8 + image_size; 

    uint8_t fileHeader[14] = { 'B', 'M', file_size, file_size >> 8, file_size >> 16, file_size >> 24, 0, 0, 0, 0, 62, 0, 0, 0 };
    uint8_t infoHeader[40] = { 40, 0, 0, 0, (uint8_t)(CANVAS_WIDTH & 0xFF), (uint8_t)((CANVAS_WIDTH >> 8) & 0xFF), 0, 0, (uint8_t)(CANVAS_HEIGHT & 0xFF), (uint8_t)((CANVAS_HEIGHT >> 8) & 0xFF), 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, image_size, image_size >> 8, image_size >> 16, image_size >> 24, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t palette[8] = { 0, 0, 0, 0, 255, 255, 255, 0 };

    fwrite(fileHeader, 1, 14, f);
    fwrite(infoHeader, 1, 40, f);
    fwrite(palette, 1, 8, f);

    for (int y = CANVAS_HEIGHT - 1; y >= 0; y--) {
        uint8_t *row_ptr = canvas_buffer + (y * CANVAS_STRIDE);
        fwrite(row_ptr, 1, row_size, f);
    }

    fclose(f);
    ESP_LOGI(TAG, "Canvas successfully saved to %s", filename);
}

void appc_sketch_save(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        appc_sketch_set_visible(false);
        lv_obj_clear_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN); 
    }
}

void appc_note_cancel(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        appc_sketch_set_visible(true); 
        appc_sketch_clear();
    }
}

void appc_note_save(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        const char * user_filename = lv_textarea_get_text(ui_TextArea3);
        if (user_filename == NULL || strlen(user_filename) == 0) {
            ESP_LOGW(TAG, "Save failed: Filename text area is empty.");
            return;
        }

        char full_path[128];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bmp", user_filename);
        ESP_LOGI(TAG, "Attempting to save sketch to full path: %s", full_path);

        save_canvas_to_bmp(full_path);

        appc_sketch_trigger_refresh();
        appc_sketch_clear();
        appc_sketch_set_visible(true); 
    }
}

bool load_bmp_to_canvas(const char *filename, uint8_t *target_buffer, lv_obj_t *target_canvas_obj) {
    if (target_buffer == NULL || target_canvas_obj == NULL) {
        ESP_LOGE(TAG, "Invalid buffer or canvas object passed to loader.");
        return false;
    }
    
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for reading!", filename);
        return false;
    }

    if (fseek(f, 62, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek past BMP headers.");
        fclose(f);
        return false;
    }

    for (int y = CANVAS_HEIGHT - 1; y >= 0; y--) {
        uint8_t *row_ptr = target_buffer + (y * CANVAS_STRIDE);
        size_t read_bytes = fread(row_ptr, 1, CANVAS_STRIDE, f);
        if (read_bytes != CANVAS_STRIDE) {
            ESP_LOGV(TAG, "Short read at row %d", y);
        }
    }

    fclose(f);
    strncpy(current_editing_file, filename, sizeof(current_editing_file) - 1);
    lv_obj_invalidate(target_canvas_obj);
    ESP_LOGI(TAG, "Successfully loaded note: %s into canvas", filename);
    return true;
}

static void dynamic_edit_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_obj_t * img_btn = lv_event_get_target(e);
        lv_obj_t * parent_btn = lv_obj_get_parent(img_btn);
        lv_obj_t * label = lv_obj_get_child(parent_btn, 0); 
        
        if(label) {
            const char * filename = lv_label_get_text(label);
            snprintf(selected_note_path, sizeof(selected_note_path), "/sdcard/%s", filename);
            ESP_LOGI(TAG, "Selected for Rename: %s", selected_note_path);
            
            lv_textarea_set_text(ui_TextArea1, filename);
            lv_obj_clear_flag(ui_NotesRenamePanel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ui_NotesRenamePanel);
        }
    }
}

static void dynamic_row_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_obj_t * row_btn = lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(row_btn, 0); 
        if(label) {
            const char * filename = lv_label_get_text(label);
            snprintf(selected_note_path, sizeof(selected_note_path), "/sdcard/%s", filename);
            ESP_LOGI(TAG, "Attempting view load path sequence: %s", selected_note_path);

            if (load_bmp_to_canvas(selected_note_path, viewer_canvas_buffer, viewer_canvas_obj)) {
                lv_obj_clear_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(ui_NotesViewPanel);
            }
        }
    }
}

// Background Task: Safely executes on Core 1 without calling any LVGL APIs directly
static void notes_loader_worker_task(void *pvParameters) {
    ESP_LOGI(TAG, "Background Worker Task Started with independent stack.");

    DIR *dir = opendir("/sdcard/");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Worker failed to open /sdcard/ directory.");
        xNotesLoaderTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct dirent *entry;
    fetched_note_count = 0;

    while ((entry = readdir(dir)) != NULL && fetched_note_count < MAX_NOTES) {
        char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".bmp") == 0) {
            strncpy(fetched_note_names[fetched_note_count], entry->d_name, sizeof(fetched_note_names[fetched_note_count]) - 1);
            fetched_note_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    closedir(dir);
    ESP_LOGI(TAG, "Worker finished scanning. Found %d notes.", fetched_note_count);

    // THREAD-SAFE STEP: Flip the signal flag for Core 0 instead of calling lv_async_call directly!
    notes_data_ready = true; 

    xNotesLoaderTaskHandle = NULL;
    vTaskDelete(NULL); 
}

// Generates an individual UI row item
static void appc_sketch_create_individual_row(int index) {
    int start_y = -113; 
    int row_height = 40; 
    // Fix positioning structure calculation to strictly follow the current sequential index
    int current_y = start_y + (index * row_height); 

    lv_obj_t * new_btn = lv_btn_create(ui_NotesDisplayPanel);
    if(new_btn == NULL) {
        ESP_LOGE(TAG, "Out of memory error while creating button layout at index %d", index);
        return;
    }
    
    lv_obj_set_height(new_btn, 40);
    lv_obj_set_width(new_btn, lv_pct(100));
    lv_obj_set_x(new_btn, 0);
    lv_obj_set_y(new_btn, current_y); 
    lv_obj_set_align(new_btn, LV_ALIGN_CENTER);
    lv_obj_add_flag(new_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(new_btn, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(new_btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(new_btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * label = lv_label_create(new_btn);
    if(label != NULL) {
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_set_align(label, LV_ALIGN_CENTER);
        lv_label_set_text(label, fetched_note_names[index]); 
        lv_obj_set_style_text_color(label, lv_color_hex(0x120000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t * img = lv_img_create(new_btn);
    if(img != NULL) {
        lv_img_set_src(img, &ui_img_note_png);
        lv_obj_set_width(img, 32);
        lv_obj_set_height(img, 32);
        lv_obj_set_x(img, -85);
        lv_obj_set_y(img, -2);
        lv_obj_set_align(img, LV_ALIGN_CENTER);
        lv_obj_add_flag(img, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        ESP_LOGE(TAG, "Out of memory error while loading raw image pointer structure!");
    }

    lv_obj_t * edit_btn = lv_imgbtn_create(new_btn);
    if(edit_btn != NULL) {
        lv_imgbtn_set_src(edit_btn, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_write_black_png, NULL);
        lv_imgbtn_set_src(edit_btn, LV_IMGBTN_STATE_PRESSED, NULL, &ui_img_write_black_png, NULL);
        lv_obj_set_width(edit_btn, 32);
        lv_obj_set_height(edit_btn, 32);
        lv_obj_set_x(edit_btn, 88);
        lv_obj_set_y(edit_btn, -2);
        lv_obj_set_align(edit_btn, LV_ALIGN_CENTER);
        lv_obj_add_event_cb(edit_btn, dynamic_edit_click_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_add_event_cb(new_btn, dynamic_row_click_cb, LV_EVENT_CLICKED, NULL);

    note_buttons[index] = new_btn;
    if (index >= note_count) {
        note_count = index + 1;
    }
    
    ESP_LOGI(TAG, "Generated Row Item for File: %s", fetched_note_names[index]);
}

// ASYNC CHAINED PAINTER: Runs contextually inside the Core 0 thread frame pool
static void appc_sketch_async_row_painter(void * param) {
    int target_index = (int)(uintptr_t)param;

    // First element initialization sequence
    if (target_index == 0) {
        ESP_LOGI(TAG, "Updating LVGL objects smoothly via safe Async Chain...");
        for(int i = 0; i < note_count; i++) {
            if(note_buttons[i] != NULL) {
                lv_obj_del(note_buttons[i]);
                note_buttons[i] = NULL;
            }
        }
        note_count = 0;
    }

    // Process precisely one item per cycle execution call frame
    if (target_index >= 0 && target_index < fetched_note_count) {
        appc_sketch_create_individual_row(target_index);
        target_index++; 

        if (target_index < fetched_note_count) {
            // Re-schedule execution on the main loop for the next row
            lv_async_call(appc_sketch_async_row_painter, (void*)(uintptr_t)target_index);
        } else {
            // Construction finished completely. Finalize views.
            if(ui_NoteTemplate != NULL) {
                lv_obj_add_flag(ui_NoteTemplate, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_invalidate(ui_NotesDisplayPanel);
            ESP_LOGI(TAG, "Async staggered screen construction completed cleanly!");
        }
    }
}

// MAIN LOOP CHECK TRIGGER: Called from main.c loop cycle right BEFORE lv_timer_handler
void appc_sketch_check_async_trigger(void) {
    if (notes_data_ready) {
        notes_data_ready = false; // Consume flag context
        
        // Safely launch async chain execution since we are running natively inside Core 0
        lv_async_call(appc_sketch_async_row_painter, (void*)(uintptr_t)0);
    }
}
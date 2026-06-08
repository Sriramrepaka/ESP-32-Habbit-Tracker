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
static volatile bool notes_data_ready = false; 

// Shared array to hold names fetched by the background worker
static char fetched_note_names[MAX_NOTES][256];
static int fetched_note_count = 0;

static lv_obj_t *note_buttons[MAX_NOTES];
static int note_count = 0;

static char current_editing_file[128] = {0};
static char selected_note_path[128] = {0};

static uint8_t *canvas_buffer = NULL;
static lv_obj_t *canvas_obj = NULL;
static lv_point_t last_point = {0, 0};

static lv_obj_t *viewer_canvas_obj = NULL;
static lv_point_t viewer_last_point = {0, 0};

static bool is_saving_active = false;

static void appc_sketch_async_row_painter(void * param);
static void notes_loader_worker_task(void *pvParameters);
void appc_sketch_set_visible(bool visible);
static void appc_sketch_create_individual_row(int index);

static bool allocate_canvas_buffer_if_needed(void) {
    if (canvas_buffer == NULL) {
        canvas_buffer = heap_caps_malloc(CANVAS_BYTE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (canvas_buffer == NULL) {
            ESP_LOGE(TAG, "DYNAMIC ERROR: Failed to allocate canvas buffer in Internal RAM!");
            return false;
        }
        ESP_LOGI(TAG, "DYNAMIC MEMORY: Canvas buffer allocated cleanly (%d bytes).", CANVAS_BYTE_SIZE);
    }
    return true;
}

static void free_canvas_buffer(void) {
    if (canvas_buffer != NULL) {
        free(canvas_buffer);
        canvas_buffer = NULL;
        ESP_LOGI(TAG, "DYNAMIC MEMORY: Canvas buffer freed completely.");
    }
}

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
        if (canvas_buffer == NULL) return; 
        
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
        if (canvas_buffer == NULL) return; 
        
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
    if (canvas_buffer != NULL && canvas_obj != NULL) {
        memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
        lv_obj_invalidate(canvas_obj);
    }
}

void appc_sketch_close(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        is_saving_active = false; 
        appc_sketch_set_visible(false);
    }
}

void appc_sketch_set_visible(bool visible) {
    if (ui_Panel4 == NULL) return;
    lv_obj_t * parent = lv_obj_get_parent(ui_Panel4);

    if (visible) {
        if (!allocate_canvas_buffer_if_needed()) return;

        // Dynamically instantiate the main canvas right as we load the screen
        if (canvas_obj == NULL) {
            canvas_obj = lv_canvas_create(parent);
            if (canvas_obj != NULL) {
                lv_coord_t x_ofs = lv_obj_get_x_aligned(ui_Panel4);
                lv_coord_t y_ofs = lv_obj_get_y_aligned(ui_Panel4);
                lv_obj_align(canvas_obj, LV_ALIGN_CENTER, x_ofs, y_ofs);
                lv_obj_set_size(canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);
                lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_event_cb(canvas_obj, sketch_event_cb, LV_EVENT_ALL, NULL);
            }
        }

        if (canvas_obj != NULL) {
            lv_canvas_set_buffer(canvas_obj, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);
            memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE); 
            lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_foreground(canvas_obj);
        }
    } else {
        if (is_saving_active) {
            ESP_LOGI(TAG, "Save protection active: Retaining buffer context.");
            return; 
        }

        // Clean removal of UI instances to prevent any scroll/render overlaps
        if (canvas_obj != NULL) {
            lv_obj_del(canvas_obj);
            canvas_obj = NULL;
        }
        if (viewer_canvas_obj != NULL) {
            lv_obj_del(viewer_canvas_obj);
            viewer_canvas_obj = NULL;
        }

        free_canvas_buffer();
    }
}

void appc_sketch_trigger_refresh(void) {
    if (xNotesLoaderTaskHandle == NULL) {
        notes_data_ready = false;
        xTaskCreatePinnedToCore(notes_loader_worker_task, "NotesWorker", 4096, NULL, 2, &xNotesLoaderTaskHandle, 1);
    }
}

static void sketch_screen_lifecycle_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_SCREEN_LOAD_START) {
        ESP_LOGI(TAG, "Entering Sketchpad Screen!");
        appc_sketch_set_visible(true);
    } 
    else if (code == LV_EVENT_SCREEN_UNLOAD_START) {
        ESP_LOGI(TAG, "Leaving Sketchpad Screen!");
        appc_sketch_set_visible(false);
    }
}

void appc_sketch_init(void) {
    // Zero canvas allocation during boot to prevent early initialization crashes
    canvas_obj = NULL;
    viewer_canvas_obj = NULL;

    if (ui_Panel4 != NULL) {
        lv_obj_add_flag(ui_Panel4, LV_OBJ_FLAG_HIDDEN); 
    }

    if (ui_Sketchpad != NULL) {
        lv_obj_add_event_cb(ui_Sketchpad, sketch_screen_lifecycle_event_cb, LV_EVENT_ALL, NULL);
    }
    
    appc_sketch_trigger_refresh();
    ESP_LOGI(TAG, "Sketchpad Initialized Successfully");
}

void save_canvas_to_bmp(const char *filename) {
    if (canvas_buffer == NULL) return;
    FILE *f = fopen(filename, "wb");
    if (f == NULL) return;

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
    
    is_saving_active = false; 
    appc_sketch_set_visible(false);
    ESP_LOGI(TAG, "Canvas successfully saved to %s", filename);
}

void appc_sketch_save(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED && canvas_obj != NULL) {
        is_saving_active = true; 
        lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN); 
    }
}

void appc_note_cancel(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        is_saving_active = false; 
        lv_obj_add_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN);
        appc_sketch_set_visible(true); 
        appc_sketch_clear();
    }
}

void appc_note_save(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        const char * user_filename = lv_textarea_get_text(ui_TextArea3);
        if (user_filename == NULL || strlen(user_filename) == 0) return;

        char full_path[128];
        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bmp", user_filename);
        save_canvas_to_bmp(full_path);

        lv_obj_add_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(ui_TextArea3, "");

        // Chained safe append logic
        if (fetched_note_count < MAX_NOTES) {
            snprintf(fetched_note_names[fetched_note_count], sizeof(fetched_note_names[fetched_note_count]), "%s.bmp", user_filename);
            appc_sketch_create_individual_row(fetched_note_count);
            fetched_note_count++;
            lv_obj_invalidate(ui_NotesDisplayPanel);
        }
    }
}

bool load_bmp_to_canvas(const char *filename, uint8_t *target_buffer, lv_obj_t *target_canvas_obj) {
    if (target_buffer == NULL || target_canvas_obj == NULL) return false;
    
    FILE *f = fopen(filename, "rb");
    if (f == NULL) return false;

    if (fseek(f, 62, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    for (int y = CANVAS_HEIGHT - 1; y >= 0; y--) {
        uint8_t *row_ptr = target_buffer + (y * CANVAS_STRIDE);
        fread(row_ptr, 1, CANVAS_STRIDE, f);
    }

    fclose(f);
    strncpy(current_editing_file, filename, sizeof(current_editing_file) - 1);
    lv_obj_invalidate(target_canvas_obj);
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
            lv_textarea_set_text(ui_TextArea1, filename);
            lv_obj_clear_flag(ui_NotesRenamePanel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ui_NotesRenamePanel);
        }
    }
}

static void dynamic_row_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED && ui_SketchViewPanel != NULL) {
        lv_obj_t * row_btn = lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(row_btn, 0); 
        if(label) {
            const char * filename = lv_label_get_text(label);
            snprintf(selected_note_path, sizeof(selected_note_path), "/sdcard/%s", filename);

            if (!allocate_canvas_buffer_if_needed()) return;

            // Instantiate viewer canvas on demand inside modal view triggers
            if (viewer_canvas_obj == NULL) {
                viewer_canvas_obj = lv_canvas_create(ui_SketchViewPanel);
                if (viewer_canvas_obj != NULL) {
                    lv_obj_set_size(viewer_canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);
                    lv_obj_align(viewer_canvas_obj, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_clear_flag(ui_SketchViewPanel, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(viewer_canvas_obj, LV_OBJ_FLAG_CLICKABLE);        
                    lv_obj_add_event_cb(viewer_canvas_obj, viewer_sketch_event_cb, LV_EVENT_ALL, NULL); 
                }
            }

            if (viewer_canvas_obj != NULL) {
                lv_canvas_set_buffer(viewer_canvas_obj, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);
                lv_obj_clear_flag(viewer_canvas_obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_background(viewer_canvas_obj); 

                if (load_bmp_to_canvas(selected_note_path, canvas_buffer, viewer_canvas_obj)) {
                    lv_obj_clear_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(ui_NotesViewPanel);
                } else {
                    appc_sketch_set_visible(false);
                }
            }
        }
    }
}

static void notes_loader_worker_task(void *pvParameters) {
    DIR *dir = opendir("/sdcard/");
    if (dir == NULL) {
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
    notes_data_ready = true; 
    xNotesLoaderTaskHandle = NULL;
    vTaskDelete(NULL); 
}

static void appc_sketch_create_individual_row(int index) {
    // Reverted to explicit coordinates manual baseline
    int start_y = -113; 
    int row_height = 40; 
    int current_y = start_y + (index * row_height); 

    lv_obj_t * new_btn = lv_btn_create(ui_NotesDisplayPanel);
    if(new_btn == NULL) return;
    
    lv_obj_set_height(new_btn, 40);
    lv_obj_set_width(new_btn, 218);
    lv_obj_align(new_btn, LV_ALIGN_CENTER, 0, current_y);
    
    lv_obj_add_flag(new_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(new_btn, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(new_btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(new_btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * label = lv_label_create(new_btn);
    if(label != NULL) {
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0); 
        lv_label_set_text(label, fetched_note_names[index]); 
        lv_obj_set_style_text_color(label, lv_color_hex(0x120000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t * edit_btn = lv_btn_create(new_btn);
    if(edit_btn != NULL) {
        lv_obj_set_width(edit_btn, 32);
        lv_obj_set_height(edit_btn, 32);
        lv_obj_align(edit_btn, LV_ALIGN_RIGHT_MID, -5, 0);

        lv_obj_set_style_bg_color(edit_btn, lv_color_hex(0xFF5F1F), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(edit_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(edit_btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(edit_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // lv_obj_set_style_pad_all(edit_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // lv_obj_t * edit_lbl = lv_label_create(edit_btn);
        // if (edit_lbl != NULL) {
        //     lv_label_set_text(edit_lbl, "E");
        //     lv_obj_align(edit_lbl, LV_ALIGN_CENTER, 0, 0);
        //     lv_obj_set_style_text_color(edit_lbl, lv_color_hex(0xFFFFFF), 0);
        // }
        
        lv_obj_add_event_cb(edit_btn, dynamic_edit_click_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_add_event_cb(new_btn, dynamic_row_click_cb, LV_EVENT_CLICKED, NULL);

    note_buttons[index] = new_btn;
    if (index >= note_count) {
        note_count = index + 1;
    }
}

static void appc_sketch_async_row_painter(void * param) {
    int target_index = (int)(uintptr_t)param;

    if (target_index == 0) {
        for(int i = 0; i < note_count; i++) {
            if(note_buttons[i] != NULL) {
                lv_obj_del(note_buttons[i]);
                note_buttons[i] = NULL;
            }
        }
        note_count = 0;
    }

    if (target_index >= 0 && target_index < fetched_note_count) {
        appc_sketch_create_individual_row(target_index);
    }
}

void appc_sketch_check_async_trigger(void) {
    static int current_paint_index = -1;

    if (notes_data_ready) {
        notes_data_ready = false; 
        current_paint_index = 0;   
    }

    if (current_paint_index >= 0) {
        if (current_paint_index < fetched_note_count) {
            lv_async_call(appc_sketch_async_row_painter, (void*)(uintptr_t)current_paint_index);
            current_paint_index++; 
        } else {
            current_paint_index = -1; 
            if(ui_NoteTemplate != NULL) {
                lv_obj_add_flag(ui_NoteTemplate, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_invalidate(ui_NotesDisplayPanel);
        }
    }
}
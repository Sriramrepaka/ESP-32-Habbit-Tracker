#include "appc_sketch.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include <dirent.h>

static const char *TAG = "APPC_SKETCH";

#define CANVAS_WIDTH  218
#define CANVAS_HEIGHT 291

#define CANVAS_STRIDE ((CANVAS_WIDTH + 7) / 8)

#define CANVAS_BYTE_SIZE (CANVAS_STRIDE * CANVAS_HEIGHT + 64)

#define MAX_NOTES 20

static lv_obj_t *note_buttons[MAX_NOTES];
static int note_count = 0;

static char current_editing_file[128] = {0};
static char selected_note_path[128] = {0};

// 1. Declare the pointer at the top level
static uint8_t canvas_buffer[CANVAS_BYTE_SIZE] __attribute__((aligned(4)));
static lv_obj_t *canvas_obj = NULL;
static lv_point_t last_point = {0, 0};

static uint8_t viewer_canvas_buffer[CANVAS_BYTE_SIZE] __attribute__((aligned(4)));
static lv_obj_t *viewer_canvas_obj = NULL;
static lv_point_t viewer_last_point = {0, 0};


void appc_sketch_clear(void);
void appc_sketch_load_all_notes(void);

static void sketch_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    //printf("Event %d\n",code);
    lv_indev_t * indev = lv_indev_get_act();
    if(!indev) return;

    lv_point_t curr_point;
    lv_indev_get_point(indev, &curr_point);
    
    // Convert to local coords relative to Panel4's position
    lv_area_t a;
    lv_obj_get_coords(canvas_obj, &a);
    curr_point.x -= a.x1;
    curr_point.y -= a.y1;

    if(code == LV_EVENT_PRESSED) {
        last_point = curr_point;
    } else if(code == LV_EVENT_PRESSING) {
        ESP_LOGV(TAG,"Drawing at Local: %d, %d, %d", curr_point.x, curr_point.y,code);
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(0x000000);
        line_dsc.width = 2;        // Slightly thicker for handwriting recognition
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
    
    // Convert touch positions to local coordinates relative to viewer_canvas_obj
    lv_area_t a;
    lv_obj_get_coords(viewer_canvas_obj, &a);
    curr_point.x -= a.x1;
    curr_point.y -= a.y1;

    if(code == LV_EVENT_PRESSED) {
        viewer_last_point = curr_point;
    } else if(code == LV_EVENT_PRESSING) {
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(0x000000); // Black ink
        line_dsc.width = 2;        
        line_dsc.round_start = true; 
        line_dsc.round_end = true;
        
        // Draw directly onto the viewer canvas object using its unique buffer coordinates
        lv_canvas_draw_line(viewer_canvas_obj, (lv_point_t[]){viewer_last_point, curr_point}, 2, &line_dsc);
        viewer_last_point = curr_point;
    }
}

void appc_sketch_close(lv_event_t * e){

    appc_sketch_clear();
    
}

void appc_sketch_set_visible(bool visible) {
    if (canvas_obj == NULL) return;

    if (visible) {
        // Unhide canvas and make it clickable again
        lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
        
        // Push it back to the foreground just in case
        lv_obj_move_foreground(canvas_obj);
    } else {
        // Hide the canvas and turn off clickability so touches pass through
        lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

void appc_sketch_init(void) {

    // 1. If it's already created, don't do it again
    if (canvas_obj != NULL) {
        return; 
    }

    // 2. If SquareLine hasn't created the panel yet, just exit quietly
    if (ui_Panel4 == NULL) {
        // No error log here, it's expected at boot
        return; 
    }

    // 3. Now it is safe to proceed
    lv_obj_t * parent = lv_obj_get_parent(ui_Panel4);
    canvas_obj = lv_canvas_create(parent);
    
    if(canvas_obj == NULL) return;

    lv_canvas_set_buffer(canvas_obj, canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);

    //lv_align_t align = lv_obj_get_content_align(ui_Panel4);
    lv_coord_t x_ofs = lv_obj_get_x_aligned(ui_Panel4);
    lv_coord_t y_ofs = lv_obj_get_y_aligned(ui_Panel4);

    lv_obj_align(canvas_obj, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_size(canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);
    
    // Position it
    //lv_obj_set_pos(canvas_obj, lv_obj_get_x(ui_Panel4), lv_obj_get_y(ui_Panel4));
    
    // Setup appearance
    // 1. Manually set the whole buffer to 1s (White)
    memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
    memset(viewer_canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);

    if (ui_SketchViewPanel != NULL) {
        viewer_canvas_obj = lv_canvas_create(ui_SketchViewPanel);
        if (viewer_canvas_obj != NULL) {
            lv_canvas_set_buffer(viewer_canvas_obj, viewer_canvas_buffer, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_ALPHA_1BIT);
            lv_obj_set_size(viewer_canvas_obj, CANVAS_WIDTH, CANVAS_HEIGHT);
            lv_obj_align(viewer_canvas_obj, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(ui_SketchViewPanel, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(ui_SketchViewPanel, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_flag(viewer_canvas_obj, LV_OBJ_FLAG_CLICKABLE);        // Enable touch registration
            lv_obj_clear_flag(viewer_canvas_obj, LV_OBJ_FLAG_SCROLLABLE);    // Stop dragging the canvas out of place
            lv_obj_add_event_cb(viewer_canvas_obj, viewer_sketch_event_cb, LV_EVENT_ALL, NULL); // Link drawing logic
            
            lv_obj_move_background(viewer_canvas_obj); // Keep delete/close UI buttons on the top layer
        }
    } else {
        ESP_LOGE(TAG, "ui_SketchViewPanel container object layout is missing!");
    }


    // 2. Tell LVGL the data changed
    lv_obj_invalidate(canvas_obj);
    lv_obj_add_flag(ui_Panel4, LV_OBJ_FLAG_HIDDEN); // This is where you crashed before
    lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(canvas_obj);

    
    lv_obj_add_event_cb(canvas_obj, sketch_event_cb, LV_EVENT_ALL, NULL);
    appc_sketch_load_all_notes();
    
    ESP_LOGI(TAG, "Sketchpad Dynamic Init Successful");
}

void appc_sketch_clear(void){
    
    memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
    lv_obj_invalidate(canvas_obj);

}

void savec_canvas_to_bmp(const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for writing! Is SD card mounted?", filename);
        return;
    }

    // BMP formats require rows to be padded to multiples of 4 bytes.
    // Our CANVAS_STRIDE is ((218 + 7) / 8) = 28 bytes. 28 is already divisible by 4!
    uint32_t row_size = CANVAS_STRIDE; 
    uint32_t image_size = row_size * CANVAS_HEIGHT;
    uint32_t file_size = 54 + 8 + image_size; // 54 byte header + 8 byte color palette + pixels

    // 1. Standard 14-byte BMP File Header
    uint8_t fileHeader[14] = {
        'B', 'M',           // Magic identifier
        file_size, file_size >> 8, file_size >> 16, file_size >> 24, // File size
        0, 0, 0, 0,         // Reserved
        62, 0, 0, 0         // Offset to image data (54 header + 8 palette)
    };

    // 2. 40-byte BMP Info Header (DIB Header)
    uint8_t infoHeader[40] = {
        40, 0, 0, 0,        // Header size

        // Width: 218 -> fits in 1 byte, but split it explicitly for safety
        (uint8_t)(CANVAS_WIDTH & 0xFF), (uint8_t)((CANVAS_WIDTH >> 8) & 0xFF), 0, 0, 
        
        // Height: 291 -> Split across two bytes (291 & 0xFF = 35) and (291 >> 8 = 1)
        (uint8_t)(CANVAS_HEIGHT & 0xFF), (uint8_t)((CANVAS_HEIGHT >> 8) & 0xFF), 0, 0,

        1, 0,               // Planes (Must be 1)
        1, 0,               // Bits per pixel (1-bit monochrome)
        0, 0, 0, 0,         // Compression (0 = None)
        image_size, image_size >> 8, image_size >> 16, image_size >> 24, // Image size
        0, 0, 0, 0,         // X pixels per meter (unspecified)
        0, 0, 0, 0,         // Y pixels per meter (unspecified)
        2, 0, 0, 0,         // Colors in palette (2 colors)
        0, 0, 0, 0          // Important colors
    };

    // 3. 8-byte Color Palette (Defines what '0' and '1' mean in RGB)
    // In BMPs, rows are written bottom-to-top, and 0 is typically black.
    // If your screen background is black (0x00), we want 0 to map to Black, 1 to White.
    uint8_t palette[8] = {
        0,   0,   0,   0,   // Index 0: Black (Blue, Green, Red, Reserved)
        255, 255, 255, 0    // Index 1: White (Blue, Green, Red, Reserved)
    };

    // Write headers to SD card
    fwrite(fileHeader, 1, 14, f);
    fwrite(infoHeader, 1, 40, f);
    fwrite(palette, 1, 8, f);

    // 4. Write Pixel Data
    // Crucial: BMP files store images from BOTTOM to TOP. 
    // We must read our canvas_buffer from the last row up to the first row.
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
        // 1. Hide the sketch canvas so it stops overlaying and blocking clicks
        appc_sketch_set_visible(false);

        // 2. Unhide your new Save / Keyboard panel (replace with your exact SquareLine variable name)
        lv_obj_clear_flag(ui_NameKeyboard, LV_OBJ_FLAG_HIDDEN); 
    }
}

void appc_note_cancel(lv_event_t * e){

    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        // 1. Re-enable sketch canvas
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
        // Optional: Trigger a visual alert/label here to tell the user to type a name
        return;
        }

        char full_path[128];

        snprintf(full_path, sizeof(full_path), "/sdcard/%s.bmp", user_filename);

        ESP_LOGI(TAG, "Attempting to save sketch to full path: %s", full_path);

        savec_canvas_to_bmp(full_path);

        // 1. Re-enable sketch canvas
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

    // Skip the 54-byte BMP header and 8-byte color palette (Total 62 bytes)
    if (fseek(f, 62, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek past BMP headers.");
        fclose(f);
        return false;
    }

    // Remember: BMP files store rows from BOTTOM to TOP.
    // We must fill our canvas_buffer from the last row up to the first row.
    for (int y = CANVAS_HEIGHT - 1; y >= 0; y--) {
        uint8_t *row_ptr = target_buffer + (y * CANVAS_STRIDE);
        size_t read_bytes = fread(row_ptr, 1, CANVAS_STRIDE, f);
        if (read_bytes != CANVAS_STRIDE) {
            ESP_LOGW(TAG, "Warning: Short read at row %d", y);
        }
    }

    fclose(f);
    
    // Save the file name globally so the "Save" and "Delete" buttons know which file to modify
    strncpy(current_editing_file, filename, sizeof(current_editing_file) - 1);

    // Tell LVGL to redraw the canvas with the newly loaded image data
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
            
            // 1. Pre-fill the Text Area with the current filename (without the path prefix)
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
        
        // Label is the 1st child (index 0)
        lv_obj_t * label = lv_obj_get_child(row_btn, 0); 
        if(label) {
            const char * filename = lv_label_get_text(label);
            snprintf(selected_note_path, sizeof(selected_note_path), "/sdcard/%s", filename);
            ESP_LOGI(TAG, "Attempting view load path sequence: %s", selected_note_path);

            
            if (load_bmp_to_canvas(selected_note_path, viewer_canvas_buffer, ui_SketchViewPanel)) {
                lv_obj_clear_flag(ui_NotesViewPanel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(ui_NotesViewPanel);
            }
        }
    }
}

void appc_sketch_load_all_notes(void) {
    // 1. Clean out previously created buttons
    for(int i = 0; i < note_count; i++) {
        if(note_buttons[i] != NULL) {
            lv_obj_del(note_buttons[i]);
            note_buttons[i] = NULL;
        }
    }
    note_count = 0;

    // 2. Open SD Card root directory
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open /sdcard directory. Is it mounted?");
        return;
    }

    struct dirent *entry;
    
    // Starting vertical position from your SquareLine code
    int start_y = -113; 
    int row_height = 40; // The height of each item block

    // 3. Scan for matching files
    while ((entry = readdir(dir)) != NULL && note_count < MAX_NOTES) {
        char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".bmp") == 0) {
            
            // Calculate where this specific row should be positioned vertically
            int current_y = start_y + (note_count * row_height);

            // --- 1. CLONE THE ROW BUTTON CONTAINER ---
            lv_obj_t * new_btn = lv_btn_create(ui_NotesDisplayPanel);
            lv_obj_set_height(new_btn, 40);
            lv_obj_set_width(new_btn, lv_pct(100));
            lv_obj_set_x(new_btn, 0);
            lv_obj_set_y(new_btn, current_y); // Dynamic positioned Y offset
            lv_obj_set_align(new_btn, LV_ALIGN_CENTER);
            lv_obj_add_flag(new_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_clear_flag(new_btn, LV_OBJ_FLAG_SCROLLABLE);
            
            // Apply matching background/borders
            lv_obj_set_style_bg_color(new_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(new_btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(new_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_side(new_btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);

            // --- 2. CLONE THE TEXT LABEL (Child Index 0) ---
            lv_obj_t * label = lv_label_create(new_btn);
            lv_obj_set_width(label, LV_SIZE_CONTENT);
            lv_obj_set_height(label, LV_SIZE_CONTENT);
            lv_obj_set_align(label, LV_ALIGN_CENTER);
            lv_label_set_text(label, entry->d_name); // Set the text to the actual SD filename
            lv_obj_set_style_text_color(label, lv_color_hex(0x120000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

            // --- 3. CLONE THE DECORATIVE NOTE IMAGE ---
            lv_obj_t * img = lv_img_create(new_btn);
            lv_img_set_src(img, &ui_img_note_png);
            lv_obj_set_width(img, 32);
            lv_obj_set_height(img, 32);
            lv_obj_set_x(img, -85);
            lv_obj_set_y(img, -2);
            lv_obj_set_align(img, LV_ALIGN_CENTER);
            lv_obj_add_flag(img, LV_OBJ_FLAG_ADV_HITTEST);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);

            // --- 4. CLONE THE EDIT IMAGE BUTTON ---
            lv_obj_t * edit_btn = lv_imgbtn_create(new_btn);
            lv_imgbtn_set_src(edit_btn, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_write_black_png, NULL);
            lv_imgbtn_set_src(edit_btn, LV_IMGBTN_STATE_PRESSED, NULL, &ui_img_write_black_png, NULL);
            lv_obj_set_width(edit_btn, 32);
            lv_obj_set_height(edit_btn, 32);
            lv_obj_set_x(edit_btn, 88);
            lv_obj_set_y(edit_btn, -2);
            lv_obj_set_align(edit_btn, LV_ALIGN_CENTER);

            // Bind click event to the edit image button instance
            lv_obj_add_event_cb(new_btn, dynamic_row_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(edit_btn, dynamic_edit_click_cb, LV_EVENT_CLICKED, NULL);

            // Store tracker pointer for cleanup routines later
            note_buttons[note_count] = new_btn;
            note_count++;
            
            ESP_LOGI(TAG, "Generated Row Item for File: %s", entry->d_name);
        }
    }

    closedir(dir);

    // Hide the static SquareLine editor blueprint element so it doesn't leave an empty row trace
    if(ui_NoteTemplate != NULL) {
        lv_obj_add_flag(ui_NoteTemplate, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Refresh parent container view
    lv_obj_invalidate(ui_NotesDisplayPanel);
}
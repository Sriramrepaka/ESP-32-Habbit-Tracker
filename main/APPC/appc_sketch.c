#include "appc_sketch.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "APPC_SKETCH";

#define CANVAS_WIDTH  218
#define CANVAS_HEIGHT 291

#define CANVAS_STRIDE ((CANVAS_WIDTH + 7) / 8)

#define CANVAS_BYTE_SIZE (CANVAS_STRIDE * CANVAS_HEIGHT + 64)



// 1. Declare the pointer at the top level
static uint8_t canvas_buffer[CANVAS_BYTE_SIZE] __attribute__((aligned(4)));
static lv_obj_t *canvas_obj = NULL;
static lv_point_t last_point = {0, 0};

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

void app_sketch_close(lv_event_t * e){

    memset(canvas_buffer, 0xFF, CANVAS_BYTE_SIZE);
    lv_obj_invalidate(canvas_obj);

}

void app_sketch_init(void) {

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

    // 2. Tell LVGL the data changed
    lv_obj_invalidate(canvas_obj);
    lv_obj_add_flag(ui_Panel4, LV_OBJ_FLAG_HIDDEN); // This is where you crashed before
    lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(canvas_obj);

    
    lv_obj_add_event_cb(canvas_obj, sketch_event_cb, LV_EVENT_ALL, NULL);
    
    ESP_LOGI(TAG, "Sketchpad Dynamic Init Successful");
}

void app_sketch_clear(void) {
    if(canvas_obj) lv_canvas_fill_bg(canvas_obj, lv_color_white(), LV_OPA_COVER);
}

void save_canvas_to_bmp(const char *filename) {
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
        CANVAS_WIDTH, CANVAS_WIDTH >> 8, 0, 0,  // Width (218)
        CANVAS_HEIGHT, CANVAS_HEIGHT >> 8, 0, 0,// Height (291)
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
    //testing git
}
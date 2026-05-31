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
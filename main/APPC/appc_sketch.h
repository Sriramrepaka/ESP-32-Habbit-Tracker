#ifndef APPC_SKETCH_H
#define APPC_SKETCH_H
#include "ui.h"


void appc_sketch_init(void);
void appc_sketch_close(lv_event_t * e);
void app_sketch_set_visible(bool visible);
void appc_sketch_save(lv_event_t * e);
void appc_note_cancel(lv_event_t * e);
void appc_note_save(lv_event_t * e);
void appc_sketch_update_loop(void);
void appc_sketch_check_async_trigger(void);

#endif
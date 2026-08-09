#ifndef APPC_H
#define APPC_H

// Include all sub-module headers here
#include "appc_wifi_connect.h"
#include "appc_sketch.h"

// Global Application Init
void appc_init(void);
void appc_update_clock_ui(void);
void appc_update_date_ui(void);
void appc_build_calendar(int start_offset, int total_days_in_month);
void appc_build_lightweight_calendar(lv_obj_t * parent, int start_offset, int total_days);

#endif
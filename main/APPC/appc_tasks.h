#ifndef APPC_TASKS_H 
#define APPC_TASKS_H 
#include "ui.h"
#include "esp_wifi.h"

typedef struct {
    uint8_t task_one   : 1; // Task 1 completed
    uint8_t task_two  : 1; // Task 2 completed
    uint8_t task_three : 1; // Task 3 completed
    uint8_t reserved   : 5;
} day_task_status_t;

void appc_set_day_tasks(lv_obj_t * parent, int day_num, int start_offset, bool task1, bool task2, bool task3);
void productivity_screen_event_cb(lv_event_t * e);
void toggle_task_and_save(int year, int month, int day, int task_num);
void appc_task_get_date(void);
void appc_task_cal_prev(lv_event_t * e);
void appc_task_cal_next(lv_event_t * e);
void appc_render_calendar(void);
void appc_tasks_init(void);

#endif /* APPC_TASKS_H */

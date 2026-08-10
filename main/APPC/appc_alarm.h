#ifndef APPC_ALARM_H
#define APPC_ALARM_H

#include "ui.h"
#include "PCM5101.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ALARMS 10

typedef struct {
    int hour;
    int minute;
    bool enabled;
    lv_obj_t *comp_obj;    // Container panel (card)
    lv_obj_t *label_obj;   // Time text label
    lv_obj_t *switch_obj;  // On/Off switch
} appc_alarm_t;

// Main module initializer (called inside appc_init)
void appc_alarm_init(void);
// Evaluates active alarms against current system time
void appc_alarm_check_time(int current_hour, int current_min, int current_sec);

#ifdef __cplusplus
}
#endif

#endif /* APPC_ALARM_H */
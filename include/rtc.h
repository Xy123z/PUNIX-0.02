#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} rtc_time_t;

void rtc_init();
void rtc_read_time(rtc_time_t* time);
uint32_t rtc_to_unix_time(rtc_time_t* time);

#endif

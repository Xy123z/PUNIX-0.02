#include "../include/rtc.h"
#include "../include/interrupt.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

extern volatile uint32_t current_unix_time;

void rtc_init() {
    rtc_time_t now;
    rtc_read_time(&now);
    current_unix_time = rtc_to_unix_time(&now) + 19800; // Apply UTC+5:30 offset
}

static int get_update_in_progress_flag() {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

static uint8_t get_rtc_register(int reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

void rtc_read_time(rtc_time_t* time) {
    while (get_update_in_progress_flag());
    time->second = get_rtc_register(0x00);
    time->minute = get_rtc_register(0x02);
    time->hour   = get_rtc_register(0x04);
    time->day    = get_rtc_register(0x07);
    time->month  = get_rtc_register(0x08);
    time->year   = get_rtc_register(0x09);

    uint8_t registerB = get_rtc_register(0x0B);

    // Convert BCD to binary if necessary
    if (!(registerB & 0x04)) {
        time->second = (time->second & 0x0F) + ((time->second / 16) * 10);
        time->minute = (time->minute & 0x0F) + ((time->minute / 16) * 10);
        time->hour   = ((time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10)) | (time->hour & 0x80);
        time->day    = (time->day & 0x0F) + ((time->day / 16) * 10);
        time->month  = (time->month & 0x0F) + ((time->month / 16) * 10);
        time->year   = (time->year & 0x0F) + ((time->year / 16) * 10);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(registerB & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }

    // Assume 21st century (year 2000+)
    time->year += 2000;
}

static int is_leap(uint32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

uint32_t rtc_to_unix_time(rtc_time_t* rtc) {
    uint32_t t = 0;
    for (uint32_t y = 1970; y < rtc->year; y++) {
        t += is_leap(y) ? 366 : 365;
    }
    for (uint32_t m = 1; m < rtc->month; m++) {
        t += days_in_month[m - 1];
        if (m == 2 && is_leap(rtc->year)) t++;
    }
    t += (rtc->day - 1);
    t = t * 86400; // seconds in a day
    t += (rtc->hour * 3600) + (rtc->minute * 60) + rtc->second;
    return t;
}

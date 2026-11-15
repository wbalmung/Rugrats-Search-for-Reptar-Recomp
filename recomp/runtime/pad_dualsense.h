/* Native Sony controller input (DualSense, DualSense Edge, DualShock 4) over USB or Bluetooth via HID. */
#pragma once
#include <stdint.h>

enum {
    DS_UP = 1 << 0, DS_DOWN = 1 << 1, DS_LEFT = 1 << 2, DS_RIGHT = 1 << 3,
    DS_CROSS = 1 << 4, DS_CIRCLE = 1 << 5, DS_SQUARE = 1 << 6, DS_TRIANGLE = 1 << 7,
    DS_L1 = 1 << 8, DS_R1 = 1 << 9, DS_L2 = 1 << 10, DS_R2 = 1 << 11,
    DS_L3 = 1 << 12, DS_R3 = 1 << 13, DS_SHARE = 1 << 14, DS_OPTIONS = 1 << 15,
    DS_PS = 1 << 16, DS_TOUCH = 1 << 17,
};

void ds_start(void);       /* background thread: finds controllers (hotplug) and reads them */
uint32_t ds_buttons(void); /* DS_* bits held on any connected controller; left stick counts as d-pad */
int ds_count(void);
void ds_rstick(int *x, int *y); /* right stick of the most deflected controller, -128..127 (x right, y down) */        /* controllers currently delivering input */

#include "hid_ev.h"

void usb_hid_task();
int usb_hid_receive_hid_event(hid_ev_t *ev);
void usb_hid_get_mouse_motion(int *dx, int *dy);

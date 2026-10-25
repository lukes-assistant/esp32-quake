#pragma once
#include <stdint.h>

#define YUVCONV_W 640
#define YUVCONV_H 480

//converts rows [row, row+rows) of the 8-bit frame to RGB888 (B,G,R bytes) in dst; returns 1 on success
typedef int (*yuvconv_chunk_fn)(const uint8_t *l8, int row, int rows, uint8_t *dst);

int yuvconv_init(void);
//8-bit frame -> UYVY... (the 2D-DMA's YUV422 byte order) display frame; blocking; 1 on success
int yuvconv_frame(const uint8_t *l8, uint8_t *yuv, yuvconv_chunk_fn ppa_chunk);
//start-up check: n RGB colours as vertical bars; the first output row (W*2 bytes) is copied to out_yuv
int yuvconv_test_rgb(const uint8_t rgb[][3], int n, uint8_t *out_yuv, uint8_t *dst_psram);

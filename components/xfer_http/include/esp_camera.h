#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

typedef enum {
    PIXFORMAT_RGB565,
    PIXFORMAT_YUV422,
    PIXFORMAT_GRAYSCALE,
    PIXFORMAT_JPEG,
    PIXFORMAT_RGB888,
    PIXFORMAT_RAW,
    PIXFORMAT_RGB444,
    PIXFORMAT_RGB555,
} pixformat_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t width;
    size_t height;
    pixformat_t format;
    struct timeval timestamp;
} camera_fb_t;

camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *fb);

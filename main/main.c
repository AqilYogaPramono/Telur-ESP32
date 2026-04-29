#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "usb_stream.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "esp_camera.h"
#include "app_httpd.h"
#include "app_wifi.h"

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define FRAME_INTERVAL FPS2INTERVAL(15)
#define FRAME_BUFFER_SIZE (55 * 1024)

static const char *TAG = "manual_mode";
static camera_fb_t s_fb = {0};
static EventGroupHandle_t s_frame_events = NULL;

#define BIT0_FRAME_REQUESTED (0x01 << 0)
#define BIT1_FRAME_READY     (0x01 << 1)
#define BIT2_FRAME_RELEASED  (0x01 << 2)

#define DAILY_CAPTURE_HOUR_WIB 13
#define DAILY_CAPTURE_MINUTE_WIB 20
#define DAILY_CAPTURE_SECOND_WIB 0

static void initialize_time_wib(void)
{
    setenv("TZ", "WIB-7", 1);
    tzset();

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static bool is_time_synced(void)
{
    time_t now = 0;
    time(&now);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2024 - 1900);
}

static int64_t seconds_until_next_daily_capture(void)
{
    time_t now = 0;
    time(&now);

    struct tm current = {0};
    localtime_r(&now, &current);

    struct tm target = current;
    target.tm_hour = DAILY_CAPTURE_HOUR_WIB;
    target.tm_min = DAILY_CAPTURE_MINUTE_WIB;
    target.tm_sec = DAILY_CAPTURE_SECOND_WIB;

    time_t target_time = mktime(&target);
    if (target_time <= now) {
        target.tm_mday += 1;
        target_time = mktime(&target);
    }
    return (int64_t)difftime(target_time, now);
}

static void scheduled_capture_task(void *arg)
{
    (void)arg;

    if (!is_time_synced()) {
        ESP_LOGI(TAG, "sinkronisasi waktu berjalan");
    }
    while (!is_time_synced()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "penjadwalan harian aktif");

    while (1) {
        int64_t wait_seconds = seconds_until_next_daily_capture();
        if (wait_seconds < 1) {
            wait_seconds = 1;
        }

        ESP_LOGI(TAG, "pengambilan terjadwal berikutnya dalam %" PRId64 " detik", wait_seconds);
        vTaskDelay(pdMS_TO_TICKS(wait_seconds * 1000));

        int job_id = -1;
        esp_err_t err = app_httpd_capture_and_upload(&job_id);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "foto terjadwal berhasil dikirim, job_id=%d", job_id);
        } else {
            ESP_LOGE(TAG, "foto terjadwal gagal: %s", esp_err_to_name(err));
        }
    }
}

camera_fb_t *esp_camera_fb_get(void)
{
    if (s_frame_events == NULL) {
        return NULL;
    }

    xEventGroupSetBits(s_frame_events, BIT0_FRAME_REQUESTED);
    EventBits_t bits = xEventGroupWaitBits(
        s_frame_events,
        BIT1_FRAME_READY,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(5000)
    );
    if ((bits & BIT1_FRAME_READY) == 0 || s_fb.buf == NULL || s_fb.len == 0) {
        ESP_LOGW(TAG, "waktu tunggu frame kamera habis");
        return NULL;
    }

    return &s_fb;
}

void esp_camera_fb_return(camera_fb_t *fb)
{
    (void)fb;
    if (s_frame_events != NULL) {
        xEventGroupSetBits(s_frame_events, BIT2_FRAME_RELEASED);
    }
}

static void camera_frame_cb(uvc_frame_t *frame, void *ptr)
{
    (void)ptr;
    if (frame == NULL || frame->data == NULL || frame->data_bytes == 0) {
        return;
    }
    if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG) {
        return;
    }
    if (s_frame_events == NULL) {
        return;
    }

    if ((xEventGroupGetBits(s_frame_events) & BIT0_FRAME_REQUESTED) == 0) {
        return;
    }

    s_fb.buf = frame->data;
    s_fb.len = frame->data_bytes;
    s_fb.width = frame->width;
    s_fb.height = frame->height;
    s_fb.format = PIXFORMAT_JPEG;
    s_fb.timestamp.tv_sec = frame->sequence;
    s_fb.timestamp.tv_usec = 0;

    xEventGroupClearBits(s_frame_events, BIT2_FRAME_RELEASED);
    xEventGroupSetBits(s_frame_events, BIT1_FRAME_READY);
    xEventGroupWaitBits(
        s_frame_events,
        BIT2_FRAME_RELEASED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(10000)
    );
    xEventGroupClearBits(s_frame_events, BIT0_FRAME_REQUESTED);
    s_fb.buf = NULL;
    s_fb.len = 0;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    app_wifi_main();
    app_httpd_main();
    initialize_time_wib();

    uint8_t *xfer_buffer_a = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    uint8_t *xfer_buffer_b = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    uint8_t *frame_buffer = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    s_frame_events = xEventGroupCreate();
    if (xfer_buffer_a == NULL || xfer_buffer_b == NULL || frame_buffer == NULL || s_frame_events == NULL) {
        ESP_LOGE(TAG, "alokasi buffer gagal");
        ESP_LOGE(TAG, "xfer_a=%p xfer_b=%p frame=%p event=%p",
                 (void *)xfer_buffer_a, (void *)xfer_buffer_b, (void *)frame_buffer, (void *)s_frame_events);
        abort();
    }

    uvc_config_t uvc_config = {
        .frame_width = FRAME_WIDTH,
        .frame_height = FRAME_HEIGHT,
        .frame_interval = FRAME_INTERVAL,
        .xfer_type = UVC_XFER_BULK,
        .xfer_buffer_size = FRAME_BUFFER_SIZE,
        .xfer_buffer_a = xfer_buffer_a,
        .xfer_buffer_b = xfer_buffer_b,
        .frame_buffer_size = FRAME_BUFFER_SIZE,
        .frame_buffer = frame_buffer,
        .frame_cb = camera_frame_cb,
        .frame_cb_arg = NULL,
    };

    ESP_ERROR_CHECK(uvc_streaming_config(&uvc_config));
    ESP_ERROR_CHECK(usb_streaming_start());
    ESP_ERROR_CHECK(usb_streaming_connect_wait(portMAX_DELAY));
    for (int i = 0; i < 30; ++i) {
        const char *ip = app_wifi_get_ip();
        if (ip[0] != '\0') {
            ESP_LOGW(TAG, "http://%s:%d", ip, CONFIG_MANUAL_HTTP_SERVER_PORT);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "mode manual siap");
    xTaskCreate(scheduled_capture_task, "scheduled_capture_task", 6144, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

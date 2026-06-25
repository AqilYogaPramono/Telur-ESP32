#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_stream.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "app_httpd.h"
#include "app_wifi.h"

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define FRAME_INTERVAL FRAME_INTERVAL_FPS_15
#define FRAME_BUFFER_SIZE (40 * 1024)
#define UVC_FORMAT_INDEX 2
#define UVC_FRAME_INDEX 5
#define UVC_VS_INTERFACE 3
#define UVC_VS_INTERFACE_ALT 3
#define UVC_VS_EP_ADDR 0x81
#define UVC_VS_EP_MPS 512
#define USB_CONNECT_WAIT_MS 45000
#define UVC_WARMUP_SETTLE_MS 800
#define UVC_WARMUP_DISCARD_FRAMES 5
#define UVC_CAPTURE_SETTLE_MS 150
#define UVC_CAPTURE_DISCARD_FRAMES 3
#define PIN_LAMPU_HPL GPIO_NUM_45
#define HPL_ON_LEVEL 0
#define HPL_OFF_LEVEL 1
#define HPL_LIGHT_SETTLE_MS 2000

static const char *TAG = "manual_mode";
static camera_fb_t s_fb = {0};
static uint8_t *s_capture_jpeg_buf = NULL;
static EventGroupHandle_t s_frame_events = NULL;
static SemaphoreHandle_t s_stream_mutex = NULL;
static uvc_config_t s_uvc_config;
static bool s_uvc_prepared = false;
static bool s_usb_stream_active = false;

#define BIT0_FRAME_REQUESTED (0x01 << 0)
#define BIT1_FRAME_READY     (0x01 << 1)

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
        pdMS_TO_TICKS(15000)
    );
    if ((bits & BIT1_FRAME_READY) == 0 || s_fb.buf == NULL || s_fb.len == 0) {
        xEventGroupClearBits(s_frame_events, BIT0_FRAME_REQUESTED | BIT1_FRAME_READY);
        ESP_LOGW(TAG, "waktu tunggu frame kamera habis");
        return NULL;
    }

    return &s_fb;
}

void esp_camera_fb_return(camera_fb_t *fb)
{
    (void)fb;
    if (s_frame_events != NULL) {
        s_fb.buf = NULL;
        s_fb.len = 0;
        xEventGroupClearBits(s_frame_events, BIT0_FRAME_REQUESTED | BIT1_FRAME_READY);
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
    if (s_frame_events == NULL || s_capture_jpeg_buf == NULL) {
        return;
    }

    if ((xEventGroupGetBits(s_frame_events) & BIT0_FRAME_REQUESTED) == 0) {
        return;
    }
    if ((xEventGroupGetBits(s_frame_events) & BIT1_FRAME_READY) != 0) {
        return;
    }
    if (frame->data_bytes > FRAME_BUFFER_SIZE) {
        ESP_LOGW(TAG, "frame MJPEG terlalu besar: %u", (unsigned)frame->data_bytes);
        return;
    }

    memcpy(s_capture_jpeg_buf, frame->data, frame->data_bytes);
    s_fb.buf = s_capture_jpeg_buf;
    s_fb.len = frame->data_bytes;
    s_fb.width = frame->width;
    s_fb.height = frame->height;
    s_fb.format = PIXFORMAT_JPEG;
    s_fb.timestamp.tv_sec = frame->sequence;
    s_fb.timestamp.tv_usec = 0;

    xEventGroupSetBits(s_frame_events, BIT1_FRAME_READY);
}

static void hpl_pin_init(void)
{
    gpio_reset_pin(PIN_LAMPU_HPL);
    gpio_set_direction(PIN_LAMPU_HPL, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(PIN_LAMPU_HPL, GPIO_FLOATING);
    gpio_hold_dis(PIN_LAMPU_HPL);
    gpio_set_drive_capability(PIN_LAMPU_HPL, GPIO_DRIVE_CAP_3);
    gpio_set_level(PIN_LAMPU_HPL, HPL_OFF_LEVEL);
}

static void hpl_on(void)
{
    gpio_hold_dis(PIN_LAMPU_HPL);
    gpio_set_direction(PIN_LAMPU_HPL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LAMPU_HPL, HPL_ON_LEVEL);
    gpio_hold_en(PIN_LAMPU_HPL);
}

static void hpl_off(void)
{
    gpio_hold_dis(PIN_LAMPU_HPL);
    gpio_set_direction(PIN_LAMPU_HPL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LAMPU_HPL, HPL_OFF_LEVEL);
}

static void discard_warmup_frames(int count)
{
    for (int i = 0; i < count; ++i) {
        camera_fb_t *wfb = esp_camera_fb_get();
        if (wfb == NULL) {
            continue;
        }
        esp_camera_fb_return(wfb);
    }
}

static void usb_stream_shutdown_locked(void)
{
    if (s_usb_stream_active) {
        usb_streaming_stop();
        s_usb_stream_active = false;
    }
}

esp_err_t esp_camera_stream_session_begin(void)
{
    if (!s_uvc_prepared || s_stream_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);

    hpl_on();
    vTaskDelay(pdMS_TO_TICKS(HPL_LIGHT_SETTLE_MS));

    if (!s_usb_stream_active) {
        esp_err_t err = uvc_streaming_config(&s_uvc_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uvc_streaming_config gagal: %s", esp_err_to_name(err));
            hpl_off();
            xSemaphoreGive(s_stream_mutex);
            return err;
        }

        err = usb_streaming_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_streaming_start gagal: %s", esp_err_to_name(err));
            usb_stream_shutdown_locked();
            hpl_off();
            xSemaphoreGive(s_stream_mutex);
            return err;
        }

        err = usb_streaming_connect_wait(USB_CONNECT_WAIT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_streaming_connect_wait gagal: %s", esp_err_to_name(err));
            usb_stream_shutdown_locked();
            hpl_off();
            xSemaphoreGive(s_stream_mutex);
            return err;
        }

        s_usb_stream_active = true;
        ESP_LOGI(TAG, "USB UVC aktif %dx%d @15fps (B525)", FRAME_WIDTH, FRAME_HEIGHT);
        vTaskDelay(pdMS_TO_TICKS(UVC_WARMUP_SETTLE_MS));
        discard_warmup_frames(UVC_WARMUP_DISCARD_FRAMES);
    } else {
        vTaskDelay(pdMS_TO_TICKS(UVC_CAPTURE_SETTLE_MS));
        discard_warmup_frames(UVC_CAPTURE_DISCARD_FRAMES);
    }

    return ESP_OK;
}

void esp_camera_stream_session_end(void)
{
    if (s_frame_events != NULL) {
        xEventGroupClearBits(s_frame_events, BIT0_FRAME_REQUESTED | BIT1_FRAME_READY);
    }
    hpl_off();
    if (s_stream_mutex != NULL) {
        xSemaphoreGive(s_stream_mutex);
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    app_wifi_main();
    app_httpd_main();
    hpl_pin_init();
    initialize_time_wib();

    uint8_t *xfer_buffer_a = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    uint8_t *xfer_buffer_b = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    uint8_t *frame_buffer = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    s_capture_jpeg_buf = (uint8_t *)malloc(FRAME_BUFFER_SIZE);
    s_frame_events = xEventGroupCreate();
    s_stream_mutex = xSemaphoreCreateMutex();
    if (xfer_buffer_a == NULL || xfer_buffer_b == NULL || frame_buffer == NULL || s_capture_jpeg_buf == NULL ||
        s_frame_events == NULL || s_stream_mutex == NULL) {
        ESP_LOGE(TAG, "alokasi buffer gagal");
        ESP_LOGE(TAG, "xfer_a=%p xfer_b=%p frame=%p capture=%p event=%p mutex=%p",
                 (void *)xfer_buffer_a, (void *)xfer_buffer_b, (void *)frame_buffer, (void *)s_capture_jpeg_buf,
                 (void *)s_frame_events, (void *)s_stream_mutex);
        abort();
    }

    s_uvc_config = (uvc_config_t){
        .frame_width = FRAME_WIDTH,
        .frame_height = FRAME_HEIGHT,
        .frame_interval = FRAME_INTERVAL,
        .format = UVC_FORMAT_MJPEG,
        .xfer_type = UVC_XFER_ISOC,
        .format_index = UVC_FORMAT_INDEX,
        .frame_index = UVC_FRAME_INDEX,
        .interface = UVC_VS_INTERFACE,
        .interface_alt = UVC_VS_INTERFACE_ALT,
        .ep_addr = UVC_VS_EP_ADDR,
        .ep_mps = UVC_VS_EP_MPS,
        .xfer_buffer_size = FRAME_BUFFER_SIZE,
        .xfer_buffer_a = xfer_buffer_a,
        .xfer_buffer_b = xfer_buffer_b,
        .frame_buffer_size = FRAME_BUFFER_SIZE,
        .frame_buffer = frame_buffer,
        .frame_cb = camera_frame_cb,
        .frame_cb_arg = NULL,
    };
    s_uvc_prepared = true;
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "app_httpd.h"
#include "esp_camera.h"

static const char *TAG = "app_httpd";
static const char *UPLOAD_URL = CONFIG_ANALYZE_EGG_ENDPOINT;
static const char *BOUNDARY = "----esp32_manual_mode_boundary";
static const size_t RESPONSE_BUF_SIZE = 1024;
static const int MAX_UPLOAD_RETRIES = 3;
static const int MAX_HTTP_REDIRECTS = 3;
static const TickType_t RETRY_DELAY_TICKS = pdMS_TO_TICKS(2000);

typedef struct {
    int request_id;
    bool running;
    bool done;
    int job_id;
    char error[96];
} manual_request_state_t;

static int s_next_request_id = 1;
static manual_request_state_t s_manual_state = {
    .request_id = 0,
    .running = false,
    .done = false,
    .job_id = -1,
    .error = {0},
};

static esp_err_t send_json(httpd_req_t *req, int status_code, const char *body)
{
    const char *status_line = "200 OK";
    switch (status_code) {
    case 200:
        status_line = "200 OK";
        break;
    case 400:
        status_line = "400 Bad Request";
        break;
    case 404:
        status_line = "404 Not Found";
        break;
    case 409:
        status_line = "409 Conflict";
        break;
    default:
        status_line = "500 Internal Server Error";
        break;
    }
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static int parse_job_id(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return -1;
    }
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return -1;
    }
    cJSON *job_id_item = cJSON_GetObjectItemCaseSensitive(root, "job_id");
    int job_id = -1;
    if (cJSON_IsNumber(job_id_item)) {
        job_id = job_id_item->valueint;
    } else if (cJSON_IsString(job_id_item) && job_id_item->valuestring != NULL) {
        job_id = atoi(job_id_item->valuestring);
    }
    cJSON_Delete(root);
    return job_id;
}

static esp_err_t upload_jpeg_and_get_job_id(const uint8_t *jpeg_data, size_t jpeg_len, int *out_job_id)
{
    char header_buf[256];
    char content_type[128];
    char tail_buf[96];

#ifdef CONFIG_ESP_TLS_USE_DS_PERIPHERAL
    ESP_LOGI(TAG, "TLS config: DS peripheral enabled");
#else
    ESP_LOGI(TAG, "TLS config: DS peripheral disabled");
#endif

#ifdef CONFIG_MBEDTLS_HARDWARE_MPI
    ESP_LOGI(TAG, "TLS config: HW MPI enabled");
#else
    ESP_LOGI(TAG, "TLS config: HW MPI disabled");
#endif

    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", BOUNDARY);
    int header_len = snprintf(
        header_buf,
        sizeof(header_buf),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"capture.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n",
        BOUNDARY
    );
    int tail_len = snprintf(tail_buf, sizeof(tail_buf), "\r\n--%s--\r\n", BOUNDARY);
    int total_len = header_len + (int)jpeg_len + tail_len;

    esp_http_client_config_t config = {
        .url = UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
    };
    char upload_url_buf[256] = {0};
    const char *upload_url = UPLOAD_URL;
    if (strncmp(UPLOAD_URL, "http://", 7) == 0 && strstr(UPLOAD_URL, "ngrok") != NULL) {
        snprintf(upload_url_buf, sizeof(upload_url_buf), "https://%s", UPLOAD_URL + 7);
        upload_url = upload_url_buf;
        ESP_LOGI(TAG, "UPLOAD_URL diset ke HTTPS dulu (hindari ngrok redirect): %s", upload_url);
        config.url = upload_url;
    } else {
        ESP_LOGI(TAG, "UPLOAD_URL digunakan: %s", upload_url);
    }

    ESP_LOGI(TAG, "UPLOAD_URL actual: %s", config.url);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "ngrok-skip-browser-warning", "true");

    int redirect_count = 0;
    while (true) {
        esp_err_t err = esp_http_client_open(client, total_len);
        if (err != ESP_OK) {
            int tls_err_code = 0;
            int tls_err_flags = 0;
            esp_http_client_get_and_clear_last_tls_error(client, &tls_err_code, &tls_err_flags);
            ESP_LOGE(TAG, "gagal membuka koneksi HTTP: %s (last tls_err=%d flags=%d)",
                     esp_err_to_name(err), tls_err_code, tls_err_flags);
            ESP_LOGE(TAG, "gagal membuka koneksi HTTP: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return err;
        }

        if (esp_http_client_write(client, header_buf, header_len) < 0) {
            ESP_LOGE(TAG, "gagal menulis header HTTP");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (esp_http_client_write(client, (const char *)jpeg_data, (int)jpeg_len) < 0) {
            ESP_LOGE(TAG, "gagal menulis data JPEG");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (esp_http_client_write(client, tail_buf, tail_len) < 0) {
            ESP_LOGE(TAG, "gagal menulis penutup multipart");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "gagal mengambil header respons");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int http_status = esp_http_client_get_status_code(client);
        if (http_status >= 300 && http_status < 400) {
            ESP_LOGW(TAG, "redirect HTTP backend=%d (panjang_konten=%d), mengikuti redirect...", http_status, content_length);

            // Hapus sisa body (biasanya kosong untuk 307), supaya request berikutnya bersih.
            int flushed = 0;
            esp_http_client_flush_response(client, &flushed);

            if (redirect_count >= MAX_HTTP_REDIRECTS) {
                ESP_LOGE(TAG, "terlalu banyak redirect (>= %d)", MAX_HTTP_REDIRECTS);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }

            esp_err_t redir_err = esp_http_client_set_redirection(client);
            if (redir_err != ESP_OK) {
                ESP_LOGE(TAG, "gagal set redirection: %s", esp_err_to_name(redir_err));
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return redir_err;
            }

            char new_url[256] = {0};
            if (esp_http_client_get_url(client, new_url, (int)sizeof(new_url)) == ESP_OK) {
                ESP_LOGW(TAG, "redirect ke URL: %s", new_url);
            }

            esp_http_client_close(client);
            redirect_count++;
            continue; // open/write ulang ke URL redirect
        }

        if (http_status != 200) {
            ESP_LOGE(TAG, "status HTTP backend=%d panjang_konten=%d", http_status, content_length);
        }

        char *response_buf = (char *)calloc(RESPONSE_BUF_SIZE, 1);
        if (response_buf == NULL) {
            ESP_LOGE(TAG, "gagal alokasi buffer respons");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }

        int read_len = esp_http_client_read_response(client, response_buf, (int)RESPONSE_BUF_SIZE - 1);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "gagal membaca respons backend");
            free(response_buf);
            return ESP_FAIL;
        }
        response_buf[read_len] = '\0';
        ESP_LOGI(TAG, "respons backend (%d): %s", http_status, response_buf);

        if (http_status != 200) {
            free(response_buf);
            return ESP_FAIL;
        }

        int job_id = parse_job_id(response_buf);
        free(response_buf);
        if (job_id <= 0) {
            ESP_LOGE(TAG, "gagal membaca job_id dari respons");
            return ESP_FAIL;
        }
        *out_job_id = job_id;
        return ESP_OK;
    }
}

static void manual_analyze_worker_task(void *arg)
{
    const int req_id = (int)(intptr_t)arg;
    int job_id = -1;
    esp_err_t err = app_httpd_capture_and_upload(&job_id);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unggah manual gagal (request_id=%d): %s", req_id, esp_err_to_name(err));
        s_manual_state.running = false;
        s_manual_state.done = true;
        s_manual_state.job_id = -1;
        snprintf(s_manual_state.error, sizeof(s_manual_state.error), "upload_failed:%s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_manual_state.running = false;
    s_manual_state.done = true;
    s_manual_state.job_id = job_id;
    s_manual_state.error[0] = '\0';
    ESP_LOGI(TAG, "unggah manual selesai request_id=%d job_id=%d", req_id, job_id);
    vTaskDelete(NULL);
}

static esp_err_t manual_analyze_handler(httpd_req_t *req)
{
    // Jika masih ada request berjalan, tolak supaya state tidak bertabrakan.
    if (s_manual_state.running) {
        char body[120];
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"busy\"}");
        return send_json(req, 409, body);
    }

    // Buat request baru dan jalankan worker di background.
    const int request_id = s_next_request_id++;
    s_manual_state.request_id = request_id;
    s_manual_state.running = true;
    s_manual_state.done = false;
    s_manual_state.job_id = -1;
    s_manual_state.error[0] = '\0';

    BaseType_t ok = xTaskCreate(
        manual_analyze_worker_task,
        "manual_analyze_worker",
        6144,
        (void *)(intptr_t)request_id,
        5,
        NULL
    );
    if (ok != pdPASS) {
        s_manual_state.running = false;
        s_manual_state.done = true;
        snprintf(s_manual_state.error, sizeof(s_manual_state.error), "task_create_failed");
        return send_json(req, 500, "{\"ok\":false,\"error\":\"task_create_failed\"}");
    }

    // Tunggu worker selesai supaya output tetap job_id (seperti sebelumnya).
    // Ini mencegah perubahan kontrak response di sisi mobile.
    const TickType_t wait_step = pdMS_TO_TICKS(200);
    const TickType_t max_wait = pdMS_TO_TICKS(120000); // 2 menit
    TickType_t waited = 0;
    while (!s_manual_state.done && waited < max_wait) {
        vTaskDelay(wait_step);
        waited += wait_step;
    }

    if (!s_manual_state.done || s_manual_state.request_id != request_id) {
        return send_json(req, 500, "{\"ok\":false,\"error\":\"timeout\"}");
    }
    if (s_manual_state.job_id > 0) {
        char body[96];
        // Output dipertahankan sesuai permintaan: hanya ok + job_id.
        snprintf(body, sizeof(body), "{\"ok\":true,\"job_id\":%d}", s_manual_state.job_id);
        return send_json(req, 200, body);
    }
    char body[160];
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"%s\"}",
             s_manual_state.error[0] ? s_manual_state.error : "upload_failed");
    return send_json(req, 500, body);
}

static esp_err_t manual_analyze_status_handler(httpd_req_t *req)
{
    // URL: /manual-analyze/requests/<id>
    const char *uri = req->uri;
    const char *prefix = "/manual-analyze/requests/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return send_json(req, 404, "{\"ok\":false,\"error\":\"not_found\"}");
    }
    int request_id = atoi(uri + strlen(prefix));
    if (request_id <= 0) {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}");
    }
    if (request_id != s_manual_state.request_id) {
        return send_json(req, 404, "{\"ok\":false,\"error\":\"request_not_found\"}");
    }

    if (s_manual_state.running) {
        return send_json(req, 200, "{\"ok\":true,\"status\":\"running\"}");
    }
    if (!s_manual_state.done) {
        return send_json(req, 200, "{\"ok\":true,\"status\":\"queued\"}");
    }
    if (s_manual_state.job_id > 0) {
        char body[120];
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"status\":\"succeeded\",\"job_id\":%d}",
                 s_manual_state.job_id);
        return send_json(req, 200, body);
    }
    char body[180];
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"status\":\"failed\",\"error\":\"%s\"}",
             s_manual_state.error[0] ? s_manual_state.error : "unknown");
    return send_json(req, 200, body);
}

static esp_err_t health_handler(httpd_req_t *req)
{
    return send_json(req, 200, "{\"ok\":true,\"service\":\"mode-manual\"}");
}

void app_httpd_main(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_MANUAL_HTTP_SERVER_PORT;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t health_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL
    };

    httpd_uri_t manual_analyze_uri = {
        .uri = "/manual-analyze",
        .method = HTTP_POST,
        .handler = manual_analyze_handler,
        .user_ctx = NULL
    };

    httpd_uri_t manual_analyze_status_uri = {
        .uri = "/manual-analyze/requests/*",
        .method = HTTP_GET,
        .handler = manual_analyze_status_handler,
        .user_ctx = NULL
    };

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_LOGI(TAG, "server HTTP manual berjalan di port %d", config.server_port);
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &health_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &manual_analyze_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &manual_analyze_status_uri));
}

esp_err_t app_httpd_capture_and_upload(int *out_job_id)
{
    if (out_job_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_camera_stream_session_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sesi kamera gagal dimulai: %s", esp_err_to_name(err));
        return err;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL || fb->buf == NULL || fb->len == 0) {
        ESP_LOGE(TAG, "frame kamera tidak tersedia");
        esp_camera_stream_session_end();
        return ESP_FAIL;
    }

    int job_id = -1;
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= MAX_UPLOAD_RETRIES; ++attempt) {
        err = upload_jpeg_and_get_job_id(fb->buf, fb->len, &job_id);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "percobaan unggah %d/%d gagal: %s", attempt, MAX_UPLOAD_RETRIES, esp_err_to_name(err));
        if (attempt < MAX_UPLOAD_RETRIES) {
            vTaskDelay(RETRY_DELAY_TICKS);
        }
    }
    esp_camera_fb_return(fb);
    esp_camera_stream_session_end();

    if (err != ESP_OK) {
        return err;
    }

    *out_job_id = job_id;
    return ESP_OK;
}

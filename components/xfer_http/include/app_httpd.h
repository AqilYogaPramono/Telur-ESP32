#ifndef APP_HTTPD_H
#define APP_HTTPD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

void app_httpd_main(void);
esp_err_t app_httpd_capture_and_upload(int *out_job_id);

#ifdef __cplusplus
}
#endif

#endif

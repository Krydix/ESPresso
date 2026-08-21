#pragma once

#include "esp_err.h"
#include "job_history.h"

esp_err_t ipp_proxy_start(void);
size_t ipp_proxy_job_snapshot(espresso_job_record_t *records, size_t capacity,
                              uint64_t now_ms);

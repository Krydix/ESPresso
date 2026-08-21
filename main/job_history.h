#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPRESSO_JOB_HISTORY_CAPACITY 16
#define ESPRESSO_JOB_HISTORY_TTL_MS (60ULL * 60ULL * 1000ULL)
#define ESPRESSO_JOB_NAME_MAX 64
#define ESPRESSO_JOB_FORMAT_MAX 48

typedef enum {
    ESPRESSO_JOB_QUEUED = 0,
    ESPRESSO_JOB_SENDING,
    ESPRESSO_JOB_COMPLETED,
    ESPRESSO_JOB_FAILED,
    ESPRESSO_JOB_CANCELLED,
} espresso_job_state_t;

typedef struct {
    uint32_t id;
    uint32_t upstream_job_id;
    bool has_upstream_job_id;
    espresso_job_state_t state;
    uint64_t started_ms;
    uint64_t updated_ms;
    size_t document_bytes;
    char name[ESPRESSO_JOB_NAME_MAX];
    char format[ESPRESSO_JOB_FORMAT_MAX];
} espresso_job_record_t;

typedef struct {
    espresso_job_record_t records[ESPRESSO_JOB_HISTORY_CAPACITY];
    size_t count;
    uint32_t next_id;
} espresso_job_history_t;

void espresso_job_history_init(espresso_job_history_t *history);
uint32_t espresso_job_history_begin(espresso_job_history_t *history,
                                    const char *name, const char *format,
                                    espresso_job_state_t state,
                                    uint64_t now_ms);
uint32_t espresso_job_history_find_upstream(const espresso_job_history_t *history,
                                            uint32_t upstream_job_id);
bool espresso_job_history_update(espresso_job_history_t *history, uint32_t id,
                                 espresso_job_state_t state,
                                 const char *format, size_t document_bytes,
                                 uint64_t now_ms);
bool espresso_job_history_attach_upstream(espresso_job_history_t *history,
                                          uint32_t id,
                                          uint32_t upstream_job_id,
                                          uint64_t now_ms);
size_t espresso_job_history_snapshot(espresso_job_history_t *history,
                                     espresso_job_record_t *records,
                                     size_t capacity, uint64_t now_ms);
const char *espresso_job_state_name(espresso_job_state_t state);

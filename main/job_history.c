#include "job_history.h"

#include <string.h>

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (!capacity) {
        return;
    }
    if (!source) {
        source = "";
    }
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void prune(espresso_job_history_t *history, uint64_t now_ms)
{
    size_t kept = 0;
    for (size_t i = 0; i < history->count; ++i) {
        const espresso_job_record_t *record = &history->records[i];
        bool expired = now_ms >= record->updated_ms &&
                       now_ms - record->updated_ms >= ESPRESSO_JOB_HISTORY_TTL_MS;
        if (!expired) {
            if (kept != i) {
                history->records[kept] = *record;
            }
            ++kept;
        }
    }
    history->count = kept;
}

static espresso_job_record_t *find_record(espresso_job_history_t *history,
                                          uint32_t id)
{
    for (size_t i = 0; i < history->count; ++i) {
        if (history->records[i].id == id) {
            return &history->records[i];
        }
    }
    return NULL;
}

void espresso_job_history_init(espresso_job_history_t *history)
{
    if (history) {
        memset(history, 0, sizeof(*history));
    }
}

uint32_t espresso_job_history_begin(espresso_job_history_t *history,
                                    const char *name, const char *format,
                                    espresso_job_state_t state,
                                    uint64_t now_ms)
{
    if (!history) {
        return 0;
    }
    prune(history, now_ms);
    if (history->count == ESPRESSO_JOB_HISTORY_CAPACITY) {
        memmove(&history->records[0], &history->records[1],
                (ESPRESSO_JOB_HISTORY_CAPACITY - 1) * sizeof(history->records[0]));
        --history->count;
    }
    espresso_job_record_t *record = &history->records[history->count++];
    memset(record, 0, sizeof(*record));
    if (++history->next_id == 0) {
        ++history->next_id;
    }
    record->id = history->next_id;
    record->state = state;
    record->started_ms = now_ms;
    record->updated_ms = now_ms;
    copy_text(record->name, sizeof(record->name),
              name && *name ? name : "Print job");
    copy_text(record->format, sizeof(record->format), format);
    return record->id;
}

uint32_t espresso_job_history_find_upstream(const espresso_job_history_t *history,
                                            uint32_t upstream_job_id)
{
    if (!history) {
        return 0;
    }
    for (size_t i = history->count; i > 0; --i) {
        const espresso_job_record_t *record = &history->records[i - 1];
        if (record->has_upstream_job_id &&
            record->upstream_job_id == upstream_job_id) {
            return record->id;
        }
    }
    return 0;
}

bool espresso_job_history_update(espresso_job_history_t *history, uint32_t id,
                                 espresso_job_state_t state,
                                 const char *format, size_t document_bytes,
                                 uint64_t now_ms)
{
    espresso_job_record_t *record = find_record(history, id);
    if (!record) {
        return false;
    }
    record->state = state;
    record->updated_ms = now_ms;
    if (format && *format) {
        copy_text(record->format, sizeof(record->format), format);
    }
    if (document_bytes) {
        record->document_bytes = document_bytes;
    }
    return true;
}

bool espresso_job_history_attach_upstream(espresso_job_history_t *history,
                                          uint32_t id,
                                          uint32_t upstream_job_id,
                                          uint64_t now_ms)
{
    espresso_job_record_t *record = find_record(history, id);
    if (!record) {
        return false;
    }
    record->upstream_job_id = upstream_job_id;
    record->has_upstream_job_id = true;
    record->updated_ms = now_ms;
    return true;
}

size_t espresso_job_history_snapshot(espresso_job_history_t *history,
                                     espresso_job_record_t *records,
                                     size_t capacity, uint64_t now_ms)
{
    if (!history) {
        return 0;
    }
    prune(history, now_ms);
    size_t count = history->count < capacity ? history->count : capacity;
    for (size_t i = 0; i < count; ++i) {
        records[i] = history->records[history->count - 1 - i];
    }
    return count;
}

const char *espresso_job_state_name(espresso_job_state_t state)
{
    switch (state) {
        case ESPRESSO_JOB_QUEUED:
            return "queued";
        case ESPRESSO_JOB_SENDING:
            return "sending";
        case ESPRESSO_JOB_COMPLETED:
            return "completed";
        case ESPRESSO_JOB_FAILED:
            return "failed";
        case ESPRESSO_JOB_CANCELLED:
            return "cancelled";
        default:
            return "unknown";
    }
}

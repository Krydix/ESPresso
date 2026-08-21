#include <assert.h>
#include <string.h>

#include "job_history.h"

static void test_lifecycle_and_newest_first_snapshot(void)
{
    espresso_job_history_t history;
    espresso_job_history_init(&history);
    uint32_t first = espresso_job_history_begin(
        &history, "Photo", "image/urf", ESPRESSO_JOB_SENDING, 1000);
    assert(first != 0);
    assert(espresso_job_history_attach_upstream(&history, first, 42, 1100));
    assert(espresso_job_history_find_upstream(&history, 42) == first);
    assert(espresso_job_history_update(&history, first, ESPRESSO_JOB_COMPLETED,
                                       NULL, 1234, 1200));
    uint32_t second = espresso_job_history_begin(
        &history, "Document", "application/pdf", ESPRESSO_JOB_QUEUED, 1300);
    assert(second != first);

    espresso_job_record_t records[ESPRESSO_JOB_HISTORY_CAPACITY];
    size_t count = espresso_job_history_snapshot(
        &history, records, ESPRESSO_JOB_HISTORY_CAPACITY, 1400);
    assert(count == 2);
    assert(records[0].id == second);
    assert(records[1].id == first);
    assert(records[1].document_bytes == 1234);
    assert(strcmp(espresso_job_state_name(records[1].state), "completed") == 0);
}

static void test_expiry_and_capacity(void)
{
    espresso_job_history_t history;
    espresso_job_history_init(&history);
    espresso_job_history_begin(&history, "Old", NULL,
                                ESPRESSO_JOB_COMPLETED, 0);
    espresso_job_record_t records[ESPRESSO_JOB_HISTORY_CAPACITY];
    assert(espresso_job_history_snapshot(
               &history, records, ESPRESSO_JOB_HISTORY_CAPACITY,
               ESPRESSO_JOB_HISTORY_TTL_MS - 1) == 1);
    assert(espresso_job_history_snapshot(
               &history, records, ESPRESSO_JOB_HISTORY_CAPACITY,
               ESPRESSO_JOB_HISTORY_TTL_MS) == 0);

    for (size_t i = 0; i < ESPRESSO_JOB_HISTORY_CAPACITY + 3; ++i) {
        espresso_job_history_begin(&history, "Job", NULL,
                                    ESPRESSO_JOB_COMPLETED, 100 + i);
    }
    assert(espresso_job_history_snapshot(
               &history, records, ESPRESSO_JOB_HISTORY_CAPACITY, 1000) ==
           ESPRESSO_JOB_HISTORY_CAPACITY);
    assert(records[0].id == history.next_id);
}

int main(void)
{
    test_lifecycle_and_newest_first_snapshot();
    test_expiry_and_capacity();
    return 0;
}

// Host tests for svc.c on the linux FreeRTOS port: jobs run in order, and a full queue refuses at once.
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "svc.h"
#include "unity.h"

static SemaphoreHandle_t s_started;
static SemaphoreHandle_t s_gate;
static SemaphoreHandle_t s_done;
static char s_order[32];

static void append(void *arg)
{
    strncat(s_order, arg, sizeof(s_order) - strlen(s_order) - 1);
}

static void finish(void *arg)
{
    xSemaphoreGive(s_done);
}

static void hold(void *arg)
{
    xSemaphoreGive(s_started);
    xSemaphoreTake(s_gate, portMAX_DELAY);
}

static void wait_done(void)
{
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)));
}

void test_svc_post_before_start_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, svc_post(append, "x"));
}

void test_svc_runs_jobs_in_order(void)
{
    s_started = xSemaphoreCreateBinary();
    s_gate = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    TEST_ASSERT_EQUAL(ESP_OK, svc_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, svc_start());
    s_order[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(append, "a"));
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(append, "b"));
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(append, "c"));
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(finish, NULL));
    wait_done();
    TEST_ASSERT_EQUAL_STRING("abc", s_order);
}

void test_svc_full_queue_refuses_without_blocking(void)
{
    s_order[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(hold, NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_started, pdMS_TO_TICKS(5000))); // hold runs, its slot is free
    static const char *const names[SVC_QUEUE_LEN] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    for (int i = 0; i < SVC_QUEUE_LEN - 1; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, svc_post(append, (void *)names[i]));
    }
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(finish, NULL)); // the eighth slot
    const TickType_t before = xTaskGetTickCount();
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, svc_post(append, "x"));
    TEST_ASSERT_UINT32_WITHIN(1, before, xTaskGetTickCount()); // no wait for room
    xSemaphoreGive(s_gate);
    wait_done();
    TEST_ASSERT_EQUAL_STRING("1234567", s_order);
}

static bool s_on_task;

static void note_task(void *arg)
{
    s_on_task = svc_on_task();
    xSemaphoreGive(s_done);
}

void test_svc_on_task_is_true_only_in_a_job(void)
{
    TEST_ASSERT_FALSE(svc_on_task());
    s_on_task = false;
    TEST_ASSERT_EQUAL(ESP_OK, svc_post(note_task, NULL));
    wait_done();
    TEST_ASSERT_TRUE(s_on_task);
}

void test_svc_run(void)
{
    RUN_TEST(test_svc_post_before_start_is_refused);
    RUN_TEST(test_svc_runs_jobs_in_order);
    RUN_TEST(test_svc_full_queue_refuses_without_blocking);
    RUN_TEST(test_svc_on_task_is_true_only_in_a_job);
}

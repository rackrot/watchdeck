#include "svc.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SLOW_JOB_MS 1000

static const char *TAG = "svc";

typedef struct {
    svc_job_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void svc_task(void *unused)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const TickType_t start = xTaskGetTickCount();
        job.fn(job.arg);
        const uint32_t ms = (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
        if (ms > SLOW_JOB_MS) {
            ESP_LOGW(TAG, "job %p took %lu ms", (void *)job.fn, (unsigned long)ms);
        }
    }
}

esp_err_t svc_start(void)
{
    if (s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    QueueHandle_t queue = xQueueCreate(SVC_QUEUE_LEN, sizeof(job_t));
    if (!queue) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = queue;
    if (xTaskCreate(svc_task, "svc", SVC_STACK, NULL, SVC_PRIO, &s_task) != pdPASS) {
        s_queue = NULL;
        vQueueDelete(queue);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool svc_on_task(void)
{
    return s_task && xTaskGetCurrentTaskHandle() == s_task;
}

esp_err_t svc_post(svc_job_t fn, void *arg)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    const job_t job = {.fn = fn, .arg = arg};
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, job %p refused", (void *)fn);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

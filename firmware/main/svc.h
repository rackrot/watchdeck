#pragma once

// The worker task for slow device jobs: card I/O, the Wi-Fi scan and the steps of a trial connect, the RSSI read,
// the USB share and image loads. Never the link task or the LVGL task.
//
// A job runs to completion and never waits on a network event; a trial connect is a state machine driven by
// Wi-Fi events, and svc runs only its short steps. A job does its I/O holding no lock, then takes the LVGL lock
// (and the model lock after it) only to hand the result to the UI, and never holds the cardfs mutex while it
// waits for the LVGL lock.

#include <stdbool.h>

#include "esp_err.h"

#define SVC_QUEUE_LEN 8
#define SVC_STACK 6144 // bytes, internal RAM
#define SVC_PRIO 3     // under the link task's 5

typedef void (*svc_job_t)(void *arg);

// Creates the task and its queue. ESP_ERR_INVALID_STATE: already started.
esp_err_t svc_start(void);

// Queues fn(arg) and never blocks. ESP_ERR_NO_MEM: the queue is full, show the caller's "busy" message.
// ESP_ERR_INVALID_STATE: svc_start hasn't run.
esp_err_t svc_post(svc_job_t fn, void *arg);

// True when called from a svc job, for asserting that a job-only function isn't called elsewhere.
bool svc_on_task(void);

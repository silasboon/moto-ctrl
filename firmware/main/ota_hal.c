#include "ota_hal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "mc_ota_hal";

/*
 * Runs esp_ota_* calls on a dedicated task at a LOWER priority than
 * app_task's safety-critical 10ms tick loop (main.c: xTaskCreate(app_task,
 * ..., 5, NULL) — this task uses priority 3). Flash sector erase can take
 * tens to a few hundred ms, long enough to starve mc_output_tick/
 * mc_lock_tick/the watchdog feed if it ran inline on whatever task calls
 * mc_session_handle() for the OTA channel (the NimBLE host task, same as
 * every other BLE write in this codebase).
 *
 * Each HAL function below enqueues a request and blocks the CALLING task
 * (NimBLE host, not app_task) on a semaphore until this lower-priority
 * task actually gets scheduled and finishes — which only happens in the
 * gaps app_task's own vTaskDelay(10ms) leaves, since app_task always
 * preempts a lower-priority task. This makes an OTA transfer slower than a
 * naive inline write would be; that's the intended trade — correctness of
 * the safety-critical loop over OTA throughput. No host test can cover this:
 * the preemption behaviour it depends on only exists under a real FreeRTOS
 * scheduler, so it is bench-only — see docs/TESTING.md / HARDWARE_TESTING.md.
 */

typedef enum { OTA_REQ_BEGIN, OTA_REQ_WRITE, OTA_REQ_FINALIZE, OTA_REQ_ABORT } ota_req_kind_t;

typedef struct {
    ota_req_kind_t kind;
    uint32_t image_size; /* BEGIN */
    uint32_t offset;     /* WRITE (informational; esp_ota_write is sequential) */
    const uint8_t *data; /* WRITE */
    size_t len;          /* WRITE */
    bool ok;              /* result, filled by ota_task before signaling done */
    SemaphoreHandle_t done;
} ota_req_t;

static QueueHandle_t s_queue;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;

static void handle_begin(ota_req_t *req)
{
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "no free OTA partition");
        req->ok = false;
        return;
    }
    esp_err_t err = esp_ota_begin(s_update_partition, req->image_size, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        req->ok = false;
        return;
    }
    req->ok = true;
}

static void handle_write(ota_req_t *req)
{
    esp_err_t err = esp_ota_write(s_ota_handle, req->data, req->len);
    req->ok = (err == ESP_OK);
    if (!req->ok) {
        ESP_LOGE(TAG, "esp_ota_write failed at offset %u: %s", (unsigned)req->offset, esp_err_to_name(err));
    }
}

static void handle_finalize(ota_req_t *req)
{
    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        req->ok = false;
        return;
    }
    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        req->ok = false;
        return;
    }
    req->ok = true;
}

static void handle_abort(ota_req_t *req)
{
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    req->ok = true;
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_req_t *req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (req->kind) {
        case OTA_REQ_BEGIN:
            handle_begin(req);
            break;
        case OTA_REQ_WRITE:
            handle_write(req);
            break;
        case OTA_REQ_FINALIZE:
            handle_finalize(req);
            break;
        case OTA_REQ_ABORT:
            handle_abort(req);
            break;
        }
        xSemaphoreGive(req->done);
    }
}

/* Blocks the calling task until ota_task has processed `req` (see header
 * comment). `req` is stack-allocated by the caller and stays valid for the
 * whole call since we block here rather than returning early. */
static bool submit(ota_req_t *req)
{
    req->done = xSemaphoreCreateBinary();
    if (req->done == NULL) {
        return false;
    }
    ota_req_t *p = req;
    if (xQueueSend(s_queue, &p, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(req->done);
        return false;
    }
    xSemaphoreTake(req->done, portMAX_DELAY);
    vSemaphoreDelete(req->done);
    return req->ok;
}

static bool hal_flash_begin(uint32_t image_size, void *ctx)
{
    (void)ctx;
    ota_req_t req = { .kind = OTA_REQ_BEGIN, .image_size = image_size };
    return submit(&req);
}

static bool hal_flash_write(uint32_t offset, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    ota_req_t req = { .kind = OTA_REQ_WRITE, .offset = offset, .data = data, .len = len };
    return submit(&req);
}

static bool hal_flash_finalize(void *ctx)
{
    (void)ctx;
    ota_req_t req = { .kind = OTA_REQ_FINALIZE };
    return submit(&req);
}

static void hal_flash_abort(void *ctx)
{
    (void)ctx;
    ota_req_t req = { .kind = OTA_REQ_ABORT };
    submit(&req);
}

static void hal_reboot(void *ctx)
{
    (void)ctx;
    esp_restart(); /* never returns */
}

void ota_hal_init(void)
{
    s_queue = xQueueCreate(4, sizeof(ota_req_t *));
    /* Priority 3: below app_task's priority 5 (main.c) -- app_task always
     * preempts an in-progress flash write within its own 10ms cadence. */
    xTaskCreate(ota_task, "mc_ota", 4096, NULL, 3, NULL);
}

mc_ota_hal_t ota_hal_get(void)
{
    mc_ota_hal_t hal = {
        .flash_begin = hal_flash_begin,
        .flash_write = hal_flash_write,
        .flash_finalize = hal_flash_finalize,
        .flash_abort = hal_flash_abort,
        .reboot = hal_reboot,
        .ctx = NULL,
    };
    return hal;
}

#include "ble_app.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_uuids.h"
#include "gatt_svr.h"

static const char *TAG = "mc_ble";

static uint8_t s_own_addr_type;
/* The shared device state, kept so a rename can rebuild the advertisement
 * from the current config without the caller passing it back in. */
static mc_app_t *s_app;

/* The rider's board name, or MOTO-CTRL if they never set one. */
static const char *device_name(void)
{
    return (s_app != NULL) ? mc_config_effective_device_name(s_app->config)
                           : MC_DEVICE_NAME_DEFAULT;
}

/* Provided by the NimBLE store-config component (NVS-backed bond storage). */
void ble_store_config_init(void);

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    /* Primary payload: flags + the 128-bit status service UUID. The two
     * together are 21 of the 31 available bytes.
     *
     * The UUID moved here from the scan response when boards became
     * renameable, and the name moved the other way. A client cannot discover
     * MOTO-CTRL boards by name any more — the name is now whatever the rider
     * typed — so the service UUID is the only stable identifier, and it has
     * to be in the primary advertisement for a scanner to filter on it
     * reliably. The name, in turn, needs the room: 29 bytes in the scan
     * response rather than the ~10 that were left beside a 128-bit UUID. */
    static const ble_uuid128_t status_uuid = MC_UUID_SVC_STATUS;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&status_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    const char *name = device_name();
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "connected; conn=%u", event->connect.conn_handle);
            gatt_svr_on_connect(event->connect.conn_handle);
        } else {
            ESP_LOGI(TAG, "connect failed (%d); re-advertising", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        gatt_svr_on_disconnect(event->disconnect.conn.conn_handle);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change; status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with this peer; delete it and allow the new
         * pairing to proceed (e.g. after the peer wiped its side). */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset; reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_app_start(mc_app_t *app)
{
    s_app = app;
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* LE Secure Connections + bonding (AGENTS.md #4). Just Works pairing
     * (no I/O) is acceptable because the real access control is the
     * application-layer Ed25519 challenge-response on top of the encrypted,
     * bonded link — a MAC/link alone is never trusted. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = gatt_svr_init(app);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
        return;
    }

    rc = ble_svc_gap_device_name_set(device_name());
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed: %d", rc);
    }

    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE host started; advertising as %s", device_name());
}

void ble_app_refresh_device_name(void)
{
    if (s_app == NULL) {
        return;
    }
    const char *name = device_name();
    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed: %d", rc);
    }

    /* The name is baked into the scan-response payload, so a running
     * advertisement keeps broadcasting the old one until it is rebuilt. Stop
     * and restart rather than trying to patch it in place.
     *
     * While a phone is connected there is no advertisement to rebuild (this
     * board advertises non-connectably only when idle), and ble_gap_adv_stop
     * returns BLE_HS_EALREADY — harmless, and the next advertise() after
     * disconnect picks the new name up. */
    ble_gap_adv_stop();
    advertise();
    ESP_LOGI(TAG, "device name is now %s", name);
}

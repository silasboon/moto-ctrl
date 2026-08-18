#include "gatt_svr.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

#include "ble_uuids.h"
#include "mc_diag.h"
#include "mc_protocol.h"

static const char *TAG = "mc_gatt";

#define MC_BLE_MAX_SESSIONS 3

static mc_app_t *s_app;

/* Per-connection session table. */
static struct {
    bool active;
    uint16_t conn_handle;
    mc_session_t session;
} s_sessions[MC_BLE_MAX_SESSIONS];

/* Characteristic value handles, filled by NimBLE at registration, used to
 * target notifications per channel. */
static uint16_t s_handle_status;
static uint16_t s_handle_auth;
static uint16_t s_handle_command;
static uint16_t s_handle_config;
static uint16_t s_handle_ota;

static mc_session_t *session_for(uint16_t conn_handle)
{
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && s_sessions[i].conn_handle == conn_handle) {
            return &s_sessions[i].session;
        }
    }
    return NULL;
}

void gatt_svr_on_connect(uint16_t conn_handle)
{
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            s_sessions[i].active = true;
            s_sessions[i].conn_handle = conn_handle;
            mc_session_init(&s_sessions[i].session);
            return;
        }
    }
    ESP_LOGW(TAG, "no free session slot for conn %u", conn_handle);
}

uint8_t gatt_svr_connection_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (s_sessions[i].active) {
            n++;
        }
    }
    return n;
}

void gatt_svr_on_disconnect(uint16_t conn_handle)
{
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && s_sessions[i].conn_handle == conn_handle) {
            s_sessions[i].active = false;
            memset(&s_sessions[i].session, 0, sizeof(s_sessions[i].session));
            return;
        }
    }
}


static uint16_t handle_for_channel(mc_channel_t ch)
{
    switch (ch) {
    case MC_CH_STATUS: return s_handle_status;
    case MC_CH_AUTH: return s_handle_auth;
    case MC_CH_COMMAND: return s_handle_command;
    case MC_CH_CONFIG: return s_handle_config;
    case MC_CH_OTA: return s_handle_ota;
    default: return 0;
    }
}

/* Send callback: notify the characteristic matching the channel. `io` is the
 * connection handle boxed in a pointer. */
/* Never fail silently here. A dropped notification looks like nothing at all
 * on the device and like an unexplained timeout in the app — exactly the
 * shape of the `configRead: timed out` bug: config_send_read() pushes ~17
 * back-to-back CONFIG_CHUNK frames with no flow control, and once the NimBLE
 * msys pool ran dry, ble_hs_mbuf_from_flat() returned NULL and the chunk
 * vanished without a trace. The pool is now sized for the worst-case burst
 * (see CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT in sdkconfig.defaults), but if it
 * ever runs dry again this has to say so out loud. */
static void ble_send(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    uint16_t conn_handle = (uint16_t)(uintptr_t)io;
    uint16_t attr_handle = handle_for_channel(ch);
    if (attr_handle == 0) {
        ESP_LOGE(TAG, "send on channel %d with no attr handle registered", (int)ch);
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "mbuf pool exhausted; DROPPED %u bytes on channel %d "
                      "(raise CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT)",
                 (unsigned)len, (int)ch);
        return;
    }
    /* notify_custom takes ownership of `om` and frees it on every path,
     * including its own error paths — do not free it here. */
    int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "notify failed rc=%d; DROPPED %u bytes on channel %d",
                 rc, (unsigned)len, (int)ch);
    }
}

void gatt_svr_push_input_event(uint8_t button, uint8_t press_type, bool action_suppressed)
{
    const uint8_t frame[4] = {
        MC_OP_INPUT_EVENT,
        button,
        press_type,
        (uint8_t)(action_suppressed ? 1 : 0),
    };
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (!s_sessions[i].active || !s_sessions[i].session.input_learn) {
            continue;
        }
        /* input_learn can only be set on an authenticated session
         * (mc_session.c gates the whole COMMAND channel), but re-check
         * rather than rely on that invariant holding across future edits. */
        if (!mc_session_is_authed(&s_sessions[i].session)) {
            continue;
        }
        ble_send((void *)(uintptr_t)s_sessions[i].conn_handle, MC_CH_COMMAND,
                 frame, sizeof(frame));
    }
}

bool gatt_svr_input_actions_suppressed(void)
{
    for (int i = 0; i < MC_BLE_MAX_SESSIONS; i++) {
        if (!s_sessions[i].active ||
            !s_sessions[i].session.input_learn_suppress_actions) {
            continue;
        }
        if (mc_session_is_authed(&s_sessions[i].session)) {
            return true;
        }
    }
    return false;
}

static void dispatch_write(uint16_t conn_handle, mc_channel_t ch, const uint8_t *data, size_t len)
{
    mc_session_t *s = session_for(conn_handle);
    if (s == NULL) {
        return;
    }
    mc_session_handle(s, s_app, ch, data, len, ble_send, (void *)(uintptr_t)conn_handle);
}

/* Access callback shared by every characteristic; the channel is passed via
 * `arg`. Writes are dispatched to the session; a read on the status char
 * returns a fresh snapshot. */
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    mc_channel_t ch = (mc_channel_t)(intptr_t)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[512];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        dispatch_write(conn_handle, ch, buf, len);
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        /* Only the status characteristic supports ATT reads: return a fresh
         * snapshot without requiring a write+notify round-trip. */
        if (ch != MC_CH_STATUS) {
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        mc_session_t *s = session_for(conn_handle);
        mc_status_t st;
        mc_status_init(&st);
        if (s_app->fill_status != NULL) {
            s_app->fill_status(&st, s_app->app_ctx);
        }
        uint16_t mask = 0;
        for (uint8_t c = 0; c < MC_OUTPUT_COUNT; c++) {
            if (mc_output_get_state(s_app->output, c)) {
                mask |= (uint16_t)(1u << c);
            }
        }
        st.output_state_mask = mask;
        if (s_app->output != NULL) {
            st.lv_cutoff_active = mc_output_lv_cutoff_active(s_app->output);
            st.hazard_active = mc_output_hazard_active(s_app->output);
        }
        if (s_app->lock != NULL) {
            st.lock_state = mc_lock_wire_state(s_app->lock);
            st.cheatcode_backoff = mc_lock_backoff_active(s_app->lock);
        }
        if (s_app->diag != NULL) {
            st.battery_mv = mc_diag_get_battery_mv(s_app->diag);
            st.output_fault_mask = mc_diag_get_fault_mask(s_app->diag);
        }
        (void)s;
        uint8_t wire[MC_STATUS_WIRE_LEN];
        mc_status_serialize(&st, wire, sizeof(wire));
        return os_mbuf_append(ctxt->om, wire, sizeof(wire)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const ble_uuid128_t uuid_svc_status = MC_UUID_SVC_STATUS;
static const ble_uuid128_t uuid_chr_status = MC_UUID_CHR_STATUS;
static const ble_uuid128_t uuid_svc_control = MC_UUID_SVC_CONTROL;
static const ble_uuid128_t uuid_chr_auth = MC_UUID_CHR_AUTH;
static const ble_uuid128_t uuid_chr_command = MC_UUID_CHR_COMMAND;
static const ble_uuid128_t uuid_svc_config = MC_UUID_SVC_CONFIG;
static const ble_uuid128_t uuid_chr_config = MC_UUID_CHR_CONFIG;
static const ble_uuid128_t uuid_svc_ota = MC_UUID_SVC_OTA;
static const ble_uuid128_t uuid_chr_ota = MC_UUID_CHR_OTA;

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_status.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &uuid_chr_status.u,
                .access_cb = chr_access,
                .arg = (void *)(intptr_t)MC_CH_STATUS,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle_status,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_control.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &uuid_chr_auth.u,
                .access_cb = chr_access,
                .arg = (void *)(intptr_t)MC_CH_AUTH,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle_auth,
            },
            {
                .uuid = &uuid_chr_command.u,
                .access_cb = chr_access,
                .arg = (void *)(intptr_t)MC_CH_COMMAND,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle_command,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_config.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &uuid_chr_config.u,
                .access_cb = chr_access,
                .arg = (void *)(intptr_t)MC_CH_CONFIG,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle_config,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_ota.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &uuid_chr_ota.u,
                .access_cb = chr_access,
                .arg = (void *)(intptr_t)MC_CH_OTA,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle_ota,
            },
            {0},
        },
    },
    {0},
};

int gatt_svr_init(mc_app_t *app)
{
    s_app = app;

    int rc = ble_gatts_count_cfg(s_services);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(s_services);
}

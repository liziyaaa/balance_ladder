#include "ble_debug.h"

#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {

static const char *TAG = "ble_debug";
static constexpr const char *kDeviceName = "BalanceLadder";

static BleCommandCallback s_command_callback = nullptr;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static uint8_t s_own_addr_type = 0;
static bool s_connected = false;
static bool s_notify_enabled = false;
static uint32_t s_last_telemetry_ms = 0;

static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kRxUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kTxUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt *ctxt, void *arg);
int gap_event_cb(ble_gap_event *event, void *arg);
void advertise();
void host_task(void *param);

static ble_gatt_chr_def s_characteristics[3] {};
static ble_gatt_svc_def s_services[2] {};

void configure_gatt_defs()
{
    std::memset(s_characteristics, 0, sizeof(s_characteristics));
    std::memset(s_services, 0, sizeof(s_services));

    s_characteristics[0].uuid = &kRxUuid.u;
    s_characteristics[0].access_cb = gatt_access_cb;
    s_characteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;

    s_characteristics[1].uuid = &kTxUuid.u;
    s_characteristics[1].access_cb = gatt_access_cb;
    s_characteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
    s_characteristics[1].val_handle = &s_tx_val_handle;

    s_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[0].uuid = &kSvcUuid.u;
    s_services[0].characteristics = s_characteristics;
}

int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }

    char command[96] {};
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    const uint16_t copy_len = len < sizeof(command) - 1 ? len : sizeof(command) - 1;
    int rc = os_mbuf_copydata(ctxt->om, 0, copy_len, command);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    command[copy_len] = '\0';

    // Sanitize input: convert common fullwidth EQUALS (U+FF1D, UTF-8 0xEF 0xBC 0x9D)
    // into ASCII '=' so that inputs from some mobile keyboards are accepted.
    char san[96] {};
    size_t si = 0;
    for (size_t i = 0; i < copy_len && si + 1 < sizeof(san);) {
        unsigned char c = static_cast<unsigned char>(command[i]);
        if (c == 0xEF && i + 2 < copy_len &&
            static_cast<unsigned char>(command[i + 1]) == 0xBC &&
            static_cast<unsigned char>(command[i + 2]) == 0x9D) {
            san[si++] = '=';
            i += 3;
            continue;
        }
        // Keep ASCII bytes as-is, drop other multi-byte sequences
        if (c < 0x80) {
            san[si++] = static_cast<char>(c);
            ++i;
        } else {
            // skip continuation/multibyte byte
            ++i;
        }
    }
    san[si] = '\0';

    ESP_LOGI(TAG, "ble recv: %s", san);

    char response[256] {};
    if (s_command_callback != nullptr) {
        s_command_callback(san, response, sizeof(response));
    }
    if (response[0] != '\0') {
        ESP_LOGI(TAG, "ble resp: %s", response);
        ble_debug_send_line(response);
    }
    return 0;
}

int gap_event_cb(ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            app_state_set_ble_connected(true);
            ESP_LOGI(TAG, "connected");
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        app_state_set_ble_connected(false);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe cur_notify=%d attr_handle=%u", event->subscribe.cur_notify,
                 static_cast<unsigned int>(event->subscribe.attr_handle));
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify != 0;
        }
        return 0;

    default:
        return 0;
    }
}

void advertise()
{
    ble_hs_adv_fields fields {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(kDeviceName));
    fields.name_len = std::strlen(kDeviceName);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed rc=%d", rc);
        return;
    }

    ble_gap_adv_params params {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &params, gap_event_cb, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    }
}

void on_sync()
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "addr infer failed rc=%d", rc);
        return;
    }
    advertise();
}

void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset reason=%d", reason);
}

void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

esp_err_t ble_debug_init(BleCommandCallback callback)
{
    s_command_callback = callback;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(kDeviceName);
    configure_gatt_defs();

    int rc = ble_gatts_count_cfg(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt count failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add failed rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool ble_debug_is_connected()
{
    return s_connected;
}

void ble_debug_send_line(const char *line)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || line == nullptr) {
        return;
    }

    os_mbuf *om = ble_hs_mbuf_from_flat(line, std::strlen(line));
    if (om == nullptr) {
        return;
    }
    int rc = ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed rc=%d", rc);
    }
}

void ble_debug_notify_telemetry(const Telemetry &telemetry)
{
    // Only send telemetry notifications if the client has enabled notify
    // for our telemetry characteristic and rate-limit the messages to
    // avoid generating frequent NimBLE "GATT procedure initiated: notify" logs.
    if (!s_connected || !s_notify_enabled) {
        return;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const uint32_t kMinIntervalMs = 100; // at most 10Hz notifications for tuning
    if (s_last_telemetry_ms != 0 && now_ms - s_last_telemetry_ms < kMinIntervalMs) {
        return;
    }

    char line[192] {};
    std::snprintf(line, sizeof(line), "T,%lu,%s,%.2f,%.2f,%.2f,%.2f,%.3f,%d,%s\n",
                  static_cast<unsigned long>(telemetry.t_ms),
                  app_state_name(telemetry.state),
                  telemetry.angle_deg,
                  telemetry.target_deg,
                  telemetry.error_deg,
                  telemetry.gyro_rate_deg_s,
                  telemetry.motor_cmd,
                  telemetry.key_pressed ? 1 : 0,
                  fault_code_name(telemetry.fault));
    ble_debug_send_line(line);
    s_last_telemetry_ms = now_ms;
}

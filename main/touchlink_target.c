/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Zigbee Touchlink Target Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <inttypes.h>

#include "light_driver.h"
#include "radio_diagnostics.h"
#include "alarm_timer.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/zcl/cluster/groups.h"

#include "touchlink_target.h"

static const char *TAG = "BILRESA_COORDINATOR";
static const char *diag_state = "BOOTING";
static int64_t pairing_deadline_us;
static unsigned joined_count, authorized_count, onoff_count;
static bool group_ready;
static bool heartbeat_started;

static const char *commission_status_name(unsigned status)
{
    static const char *names[] = {
        "SUCCESS", "IN_PROGRESS", "NOT_AA_CAPABLE", "NO_NETWORK",
        "TARGET_FAILURE", "FORMATION_FAILURE", "NO_IDENTIFY_QUERY_RESPONSE",
        "BINDING_TABLE_FULL", "NO_SCAN_RESPONSE", "NOT_PERMITTED",
        "TCLK_EX_FAILURE", "NOT_ON_A_NETWORK", "ON_A_NETWORK", "CANCELLED",
        "DEV_ANNCE_SEND_FAILURE"
    };
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}

static void diagnostic_heartbeat(alarm_timer_arg_t arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    int64_t remaining = (pairing_deadline_us - esp_timer_get_time()) / 1000000;
    if (remaining < 0) remaining = 0;
    const bool factory_new = ezb_bdb_is_factory_new();
    if (pairing_deadline_us && !remaining && !onoff_count && !joined_count) diag_state = "JOIN_WINDOW_CLOSED";
    ESP_LOGI("DIAG", "state=%s factory_new=%d channel=%u pan=0x%04x short=0x%04x remaining_s=%" PRId64
             " joined=%u authorized=%u onoff=%u group_ready=%u mode=coordinator uptime_s=%" PRId64,
             diag_state, factory_new, ezb_nwk_get_current_channel(), ezb_nwk_get_panid(),
             ezb_nwk_get_short_address(), remaining, joined_count, authorized_count, onoff_count, group_ready,
             esp_timer_get_time() / 1000000);
    esp_zigbee_lock_release();
    alarm_timer_schedule(diagnostic_heartbeat, 0, 5000);
}

static void configure_local_group(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (!group_ready) {
        ezb_zcl_groups_add_group_cmd_t cmd={
            .cmd_ctrl={.dst_addr=EZB_ADDRESS_SHORT(0), .dst_ep=1, .src_ep=1},
            .payload={.group_id=0x549a, .group_name=""},
        };
        ezb_err_t err=ezb_zcl_groups_add_group_cmd_req(&cmd);
        ESP_LOGI("DIAG", "GROUP_ADD_REQUEST group=0x549a endpoint=1 attempt=%lu result=0x%x", (unsigned long)arg+1, (unsigned)err);
        if (arg<4) alarm_timer_schedule(configure_local_group, arg+1, 2000);
        else ESP_LOGW("DIAG", "GROUP_SETUP awaiting final response; inspect group_ready");
    }
    esp_zigbee_lock_release();
}

static void coordinator_ready(void)
{
    alarm_timer_schedule(configure_local_group, 0, 100);
    ezb_err_t err=ezb_bdb_open_network(180);
    ESP_LOGI("DIAG", "JOIN_OPEN_REQUEST duration_s=180 result=0x%x", (unsigned)err);
    if (err) diag_state="START_FAILED";
}

esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;

    ESP_RETURN_ON_FALSE(!is_inited, ESP_OK, TAG, "Deferred driver already initialized");

    light_driver_init(false);
    is_inited = true;

    return is_inited ? ESP_OK : ESP_FAIL;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);
    ESP_LOGI("DIAG", "APP_SIGNAL name=%s type=0x%02x", ezb_app_signal_to_string(signal_type), signal_type);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        ESP_LOGI("DIAG", "STARTUP status=%s code=0x%02x", commission_status_name(status), status);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                diag_state="FORMING_NETWORK";
                ezb_err_t err=ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
                ESP_LOGI("DIAG", "FORMATION_START result=0x%x", (unsigned)err);
                if (err) diag_state="START_FAILED";
            } else {
                ESP_LOGI("DIAG", "COORDINATOR_RESTORED channel=%u short=0x%04x", ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
                coordinator_ready();
            }
            if (!heartbeat_started) {
                heartbeat_started = true;
                alarm_timer_schedule(diagnostic_heartbeat, 0, 1000);
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), retry again", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status=*(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI("DIAG", "FORMATION status=%s code=0x%02x channel=%u pan=0x%04x short=0x%04x", commission_status_name(status), status, ezb_nwk_get_current_channel(), ezb_nwk_get_panid(), ezb_nwk_get_short_address());
        if (status==EZB_BDB_STATUS_SUCCESS) coordinator_ready();
        else { diag_state="FORMATION_FAILED"; alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_FORMATION, 3000); }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *dev=ezb_app_signal_get_params(app_signal);
        joined_count++;
        diag_state="DEVICE_ANNOUNCED";
        ESP_LOGI("DIAG", "DEVICE_ANNOUNCED ieee=0x%016" PRIx64 " short=0x%04x capabilities=0x%02x count=%u", dev->device_addr.u64, dev->short_addr, dev->capability, joined_count);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *dev=ezb_app_signal_get_params(app_signal);
        if (dev->status==EZB_ZDO_AUTH_STATUS_SUCCESS) authorized_count++;
        ESP_LOGI("DIAG", "DEVICE_AUTHORIZED ieee=0x%016" PRIx64 " short=0x%04x type=%u status=%u", dev->device_addr.u64, dev->short_addr, dev->type, dev->status);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *dev=ezb_app_signal_get_params(app_signal);
        ESP_LOGI("DIAG", "DEVICE_UPDATE ieee=0x%016" PRIx64 " short=0x%04x status=%u tc_action=%u parent=0x%04x", dev->device_addr.u64, dev->short_addr, dev->status, dev->tc_action, dev->parent_short);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        diag_state = "LEFT_NETWORK";
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network successfully with type(0x%02x)", leave_params->leave_type);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        if (duration) {
            pairing_deadline_us=esp_timer_get_time()+(int64_t)duration*1000000;
            if (!joined_count && !onoff_count) diag_state="JOIN_OPEN";
            ESP_LOGI("DIAG", "JOIN_READY duration_s=%u channel=%u group=0x549a", duration, ezb_nwk_get_current_channel());
        } else {
            ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", ezb_nwk_get_panid());
        }
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void zcl_core_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    ESP_LOGI("DIAG", "ATTRIBUTE endpoint=%u cluster=0x%04x attr=0x%04x type=0x%02x size=%u status=0x%02x",
             message->info.dst_ep, message->info.cluster_id, message->in.attribute.id,
             message->in.attribute.data.type, message->in.attribute.data.size, message->info.status);
    ESP_LOGI(TAG, "ZCL SetAttributeValue message for endpoint(%d) cluster(0x%04x) %s with status(0x%02x)", message->info.dst_ep,
             message->info.cluster_id, message->info.cluster_role == EZB_ZCL_CLUSTER_SERVER ? "server" : "client",
             message->info.status);
    if (message->info.status == 0 &&
        message->info.dst_ep == ESP_ZIGBEE_HA_ON_OFF_LIGHT_EP_ID &&
        message->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF &&
        message->in.attribute.id == 0x0000 &&
        message->in.attribute.data.size == 1 &&
        message->in.attribute.data.value != NULL) {
        light_driver_set_power(*(uint8_t *)message->in.attribute.data.value);
        onoff_count++;
        diag_state = "CONTROL_RECEIVED";
        ESP_LOGI(TAG, "Set On/Off: %d", *(uint8_t *)message->in.attribute.data.value);
    } else {
        ESP_LOGW(TAG, "Unsupported cluster ID(0x%04x)", message->info.cluster_id);
    }
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        zcl_core_set_attr_value_handler(message);
        break;
    case EZB_ZCL_CORE_GROUPS_ADD_GROUP_RSP_CB_ID: {
        const ezb_zcl_groups_add_group_rsp_message_t *rsp=message;
        ESP_LOGI("DIAG", "GROUP_ADD_RESPONSE group=0x%04x status=0x%02x", rsp->in.group_id, rsp->in.status);
        if (rsp->in.status==0 && rsp->in.group_id==0x549a) {
            ezb_zcl_groups_view_group_cmd_t cmd={.cmd_ctrl={.dst_addr=EZB_ADDRESS_SHORT(0), .dst_ep=1, .src_ep=1}, .payload={.group_id=0x549a}};
            ezb_zcl_groups_view_group_cmd_req(&cmd);
        }
    } break;
    case EZB_ZCL_CORE_GROUPS_VIEW_GROUP_RSP_CB_ID: {
        const ezb_zcl_groups_view_group_rsp_message_t *rsp=message;
        if (rsp->in.status==0 && rsp->in.group_id==0x549a) group_ready=true;
        ESP_LOGI("DIAG", "GROUP_VERIFIED group=0x%04x status=0x%02x ready=%u", rsp->in.group_id, rsp->in.status, group_ready);
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGI(TAG, "Received ZCL Default Response with status(0x%02x)", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGW(TAG, "ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

esp_err_t esp_zigbee_create_zha_on_off_light_device(void)
{
    ezb_af_device_desc_t          dev_desc   = ezb_af_create_device_desc();
    ezb_zha_on_off_light_config_t light_cfg  = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t              ep_desc    = ezb_zha_create_on_off_light(ESP_ZIGBEE_HA_ON_OFF_LIGHT_EP_ID, &light_cfg);
    ezb_zcl_cluster_desc_t        basic_desc = {0};

    basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ESP_MODEL_IDENTIFIER);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_groups_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(1U<<11));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_LOGI("DIAG", "COORDINATOR_CONFIG channel=11 endpoint=1 group=0x549a storage=zc_store");
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));

    return ESP_OK;
}

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());

    ESP_ERROR_CHECK(esp_zigbee_create_zha_on_off_light_device());

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();

    vTaskDelete(NULL);
}

void app_main(void)
{
    // Seeed XIAO antenna switch: enable, select onboard ceramic antenna.
    const gpio_config_t rf_pins = {
        .pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rf_pins));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 0));
    ESP_ERROR_CHECK(nvs_flash_init_partition("zc_store"));
    radio_diagnostics_init();

    ESP_LOGI(TAG, "Start ESP Zigbee Stack");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}

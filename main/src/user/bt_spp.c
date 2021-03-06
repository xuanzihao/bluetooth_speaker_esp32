/*
 * bt_spp.c
 *
 *  Created on: 2019-07-03 15:48
 *      Author: Jack Chen <redchenjs@live.com>
 */

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_a2dp_api.h"
#include "esp_spp_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/os.h"
#include "core/app.h"
#include "chip/i2s.h"
#include "user/led.h"
#include "user/vfx.h"
#include "user/bt_av.h"
#include "user/bt_app.h"
#include "user/ble_app.h"
#include "user/ble_gatts.h"
#include "user/audio_input.h"

#define BT_SPP_TAG "bt_spp"
#define BT_OTA_TAG "bt_ota"

#ifdef CONFIG_ENABLE_OTA_OVER_SPP
uint32_t spp_conn_handle = 0;

#ifdef CONFIG_ENABLE_VFX
    static vfx_config_t *vfx = NULL;
    static uint8_t vfx_prev_mode = 0;
#endif
#ifndef CONFIG_AUDIO_INPUT_NONE
    static uint8_t ain_prev_mode = 0;
#endif

static esp_bd_addr_t spp_remote_bda = {0};

static long image_length = 0;
static long data_recv = 0;

static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t update_handle = 0;

static const char fw_cmd[][32] = {
    "FW+RST\r\n",       // Reset Device
    "FW+RAM?\r\n",      // Get RAM Information
    "FW+VER?\r\n",      // Get Firmware Version
    "FW+UPD:%ld\r\n",   // Update Device Firmware
};

static const char rsp_str[][32] = {
    "OK\r\n",           // OK
    "DONE\r\n",         // Done
    "ERROR\r\n",        // Error
    "LOCKED\r\n",       // Locked
    "RAM:%u\r\n",       // RAM Info
    "VER:%s\r\n",       // Firmware Ver
};

static const char *s_spp_conn_state_str[] = {"disconnected", "connected"};

static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

void bt_app_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
#ifdef CONFIG_ENABLE_VFX
    vfx = vfx_get_conf();
#endif
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(sec_mask, role_slave, 0, CONFIG_BT_SPP_SERVER_NAME);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        break;
    case ESP_SPP_OPEN_EVT:
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(BT_SPP_TAG, "SPP connection state: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 s_spp_conn_state_str[0],
                 spp_remote_bda[0], spp_remote_bda[1], spp_remote_bda[2],
                 spp_remote_bda[3], spp_remote_bda[4], spp_remote_bda[5]);

        memset(&spp_remote_bda, 0x00, sizeof(esp_bd_addr_t));

        spp_conn_handle = 0;

        if (update_handle) {
            esp_ota_end(update_handle);
            update_handle = 0;

            image_length = 0;

            i2s_output_init();
#ifndef CONFIG_AUDIO_INPUT_NONE
            audio_input_set_mode(ain_prev_mode);
#endif
#ifdef CONFIG_ENABLE_VFX
            vfx->mode = vfx_prev_mode;
            vfx_set_conf(vfx);
#endif

#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
            esp_ble_gap_start_advertising(&adv_params);
#endif
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

            xEventGroupSetBits(user_event_group, KEY_SCAN_RUN_BIT);
        }

#ifdef CONFIG_ENABLE_LED
        led_set_mode(3);
#endif

        xEventGroupSetBits(user_event_group, BT_SPP_IDLE_BIT);
        break;
    case ESP_SPP_START_EVT:
        break;
    case ESP_SPP_CL_INIT_EVT:
        break;
    case ESP_SPP_DATA_IND_EVT:
        if (update_handle == 0) {
            if (strncmp(fw_cmd[0], (const char *)param->data_ind.data, strlen(fw_cmd[0])) == 0) {
                ESP_LOGI(BT_SPP_TAG, "GET command: FW+RST");

                xEventGroupSetBits(user_event_group, BT_OTA_LOCKED_BIT);

                memset(&last_remote_bda, 0x00, sizeof(esp_bd_addr_t));
                app_setenv("LAST_REMOTE_BDA", &last_remote_bda, sizeof(esp_bd_addr_t));

#ifdef CONFIG_ENABLE_VFX
                vfx->mode = VFX_MODE_IDX_OFF;
                vfx_set_conf(vfx);
#endif
#ifndef CONFIG_AUDIO_INPUT_NONE
                audio_input_set_mode(0);
#endif

                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

                EventBits_t uxBits = xEventGroupGetBits(user_event_group);
                if (!(uxBits & BT_A2DP_IDLE_BIT)) {
                    esp_a2d_sink_disconnect(a2d_remote_bda);
                }
#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
                if (!(uxBits & BLE_GATTS_IDLE_BIT)) {
                    esp_ble_gatts_close(gl_profile_tab[PROFILE_A_APP_ID].gatts_if,
                                        gl_profile_tab[PROFILE_A_APP_ID].conn_id);
                    esp_ble_gatts_close(gl_profile_tab[PROFILE_B_APP_ID].gatts_if,
                                        gl_profile_tab[PROFILE_B_APP_ID].conn_id);
                }
                os_power_restart_wait(BT_SPP_IDLE_BIT | BT_A2DP_IDLE_BIT | BLE_GATTS_IDLE_BIT);
#else
                os_power_restart_wait(BT_SPP_IDLE_BIT | BT_A2DP_IDLE_BIT);
#endif

                esp_spp_disconnect(param->write.handle);
            } else if (strncmp(fw_cmd[1], (const char *)param->data_ind.data, strlen(fw_cmd[1])) == 0) {
                ESP_LOGI(BT_SPP_TAG, "GET command: FW+RAM?");

                char str_buf[40] = {0};
                snprintf(str_buf, sizeof(str_buf), rsp_str[4], heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

                esp_spp_write(param->write.handle, strlen(str_buf), (uint8_t *)str_buf);
            } else if (strncmp(fw_cmd[2], (const char *)param->data_ind.data, strlen(fw_cmd[2])) == 0) {
                ESP_LOGI(BT_SPP_TAG, "GET command: FW+VER?");

                char str_buf[40] = {0};
                snprintf(str_buf, sizeof(str_buf), rsp_str[5], app_get_version());

                esp_spp_write(param->write.handle, strlen(str_buf), (uint8_t *)str_buf);
            } else if (strncmp(fw_cmd[3], (const char *)param->data_ind.data, 7) == 0) {
                sscanf((const char *)param->data_ind.data, fw_cmd[3], &image_length);
                ESP_LOGI(BT_SPP_TAG, "GET command: FW+UPD:%ld", image_length);

                EventBits_t uxBits = xEventGroupGetBits(user_event_group);
                if (image_length != 0 && !(uxBits & BT_OTA_LOCKED_BIT)
#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
                    && (uxBits & BLE_GATTS_IDLE_BIT)
#endif
                ) {
                    xEventGroupClearBits(user_event_group, KEY_SCAN_RUN_BIT);

                    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
                    esp_ble_gap_stop_advertising();
#endif

#ifdef CONFIG_ENABLE_VFX
                    vfx_prev_mode = vfx->mode;
                    vfx->mode = VFX_MODE_IDX_OFF;
                    vfx_set_conf(vfx);
#endif
#ifndef CONFIG_AUDIO_INPUT_NONE
                    ain_prev_mode = audio_input_get_mode();
                    audio_input_set_mode(0);
#endif
#ifdef CONFIG_ENABLE_AUDIO_PROMPT
                    xEventGroupWaitBits(
                        user_event_group,
                        AUDIO_PLAYER_IDLE_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY
                    );
#endif
                    i2s_output_deinit();

                    update_partition = esp_ota_get_next_update_partition(NULL);
                    if (update_partition != NULL) {
                        ESP_LOGI(BT_OTA_TAG, "writing to partition subtype %d at offset 0x%x",
                                 update_partition->subtype, update_partition->address);
                    } else {
                        ESP_LOGE(BT_OTA_TAG, "no ota partition to write");
                        goto err0;
                    }

                    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                    if (err != ESP_OK) {
                        ESP_LOGE(BT_OTA_TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        goto err0;
                    }

                    data_recv = 0;

                    esp_spp_write(param->write.handle, strlen(rsp_str[0]), (uint8_t *)rsp_str[0]);
                } else if (uxBits & BT_OTA_LOCKED_BIT
#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
                    || !(uxBits & BLE_GATTS_IDLE_BIT)
#endif
                ) {
                    esp_spp_write(param->write.handle, strlen(rsp_str[3]), (uint8_t *)rsp_str[3]);
                } else {
                    esp_spp_write(param->write.handle, strlen(rsp_str[2]), (uint8_t *)rsp_str[2]);
                }
            } else {
                esp_spp_write(param->write.handle, strlen(rsp_str[2]), (uint8_t *)rsp_str[2]);
            }
        } else {
            esp_err_t err = esp_ota_write(update_handle, (const void *)param->data_ind.data, param->data_ind.len);
            if (err != ESP_OK) {
                ESP_LOGE(BT_OTA_TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
                goto err1;
            }

            data_recv += param->data_ind.len;
            ESP_LOGD(BT_OTA_TAG, "have written image length %ld", data_recv);

            if (data_recv == image_length) {
                esp_err_t err = esp_ota_end(update_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(BT_OTA_TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
                    goto err0;
                }

                err = esp_ota_set_boot_partition(update_partition);
                if (err != ESP_OK) {
                    ESP_LOGE(BT_OTA_TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
                    goto err0;
                }

                update_handle = 0;

                esp_spp_write(param->write.handle, strlen(rsp_str[1]), (uint8_t *)rsp_str[1]);
            } else if (data_recv > image_length) {
err1:
                esp_ota_end(update_handle);
                update_handle = 0;
err0:
                esp_spp_write(param->write.handle, strlen(rsp_str[2]), (uint8_t *)rsp_str[2]);

                i2s_output_init();
#ifndef CONFIG_AUDIO_INPUT_NONE
                audio_input_set_mode(ain_prev_mode);
#endif
#ifdef CONFIG_ENABLE_VFX
                vfx->mode = vfx_prev_mode;
                vfx_set_conf(vfx);
#endif

#ifdef CONFIG_ENABLE_BLE_CONTROL_IF
                esp_ble_gap_start_advertising(&adv_params);
#endif
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

                xEventGroupSetBits(user_event_group, KEY_SCAN_RUN_BIT);
            }
        }
        break;
    case ESP_SPP_CONG_EVT:
        break;
    case ESP_SPP_WRITE_EVT:
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        xEventGroupClearBits(user_event_group, BT_SPP_IDLE_BIT);

        spp_conn_handle = param->srv_open.handle;

        memcpy(&spp_remote_bda, param->srv_open.rem_bda, sizeof(esp_bd_addr_t));

        ESP_LOGI(BT_SPP_TAG, "SPP connection state: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 s_spp_conn_state_str[1],
                 spp_remote_bda[0], spp_remote_bda[1], spp_remote_bda[2],
                 spp_remote_bda[3], spp_remote_bda[4], spp_remote_bda[5]);

#ifdef CONFIG_ENABLE_LED
        led_set_mode(7);
#endif

        break;
    default:
        break;
    }
}
#endif

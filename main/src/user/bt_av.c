/*
 * bt_av.c
 *
 *  Created on: 2019-04-29 12:33
 *      Author: Jack Chen <redchenjs@live.com>
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"

#include "core/os.h"
#include "core/app.h"
#include "chip/i2s.h"
#include "user/led.h"
#include "user/vfx.h"
#include "user/bt_av.h"
#include "user/bt_app.h"
#include "user/ble_app.h"
#include "user/bt_app_core.h"
#include "user/audio_input.h"
#include "user/audio_player.h"

#define BT_A2D_TAG   "bt_a2d"
#define BT_RC_CT_TAG "bt_rc_ct"
#define BT_RC_TG_TAG "bt_rc_tg"
#define BT_RC_RN_TAG "bt_rc_rn"

// AVRCP used transaction label
#define APP_RC_CT_TL_GET_CAPS            (0)
#define APP_RC_CT_TL_GET_META_DATA       (1)
#define APP_RC_CT_TL_RN_TRACK_CHANGE     (2)
#define APP_RC_CT_TL_RN_PLAYBACK_CHANGE  (3)
#define APP_RC_CT_TL_RN_PLAY_POS_CHANGE  (4)

/* a2dp event handler */
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param);
/* avrc CT event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);
/* avrc TG event handler */
static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param);

static const char *s_a2d_conn_state_str[] = {"disconnected", "connecting", "connected", "disconnecting"};
static const char *s_a2d_audio_state_str[] = {"suspended", "stopped", "started"};

static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;

static int sample_rate = 16000;

esp_bd_addr_t a2d_remote_bda = {0};

/* callback for A2DP sink */
void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT: {
        bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    }
    default:
        break;
    }
}

void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
#if !defined(CONFIG_AUDIO_INPUT_NONE) || defined(CONFIG_ENABLE_AUDIO_PROMPT) || defined(CONFIG_ENABLE_VFX)
    EventBits_t uxBits = xEventGroupGetBits(user_event_group);
#endif

#ifdef CONFIG_ENABLE_AUDIO_PROMPT
    if (uxBits & AUDIO_PLAYER_RUN_BIT) {
        return;
    }
#endif

    i2s_output_set_sample_rate(sample_rate);

    size_t bytes_written = 0;
    i2s_write(CONFIG_AUDIO_OUTPUT_I2S_NUM, data, len, &bytes_written, portMAX_DELAY);

#ifndef CONFIG_AUDIO_INPUT_NONE
    if (uxBits & AUDIO_INPUT_RUN_BIT) {
        return;
    }
#endif

#ifdef CONFIG_ENABLE_VFX
    if (!(uxBits & VFX_FFT_NULL_BIT)) {
        return;
    }

    // Copy data to FFT input buffer
    uint32_t idx = 0;

#ifdef CONFIG_BT_AUDIO_FFT_ONLY_LEFT
    int16_t data_l = 0;
    for (uint16_t k=0; k<FFT_N; k++,idx+=4) {
        data_l = data[idx+1] << 8 | data[idx];

        vfx_fft_input[k] = (float)data_l;
    }
#elif defined(CONFIG_BT_AUDIO_FFT_ONLY_RIGHT)
    int16_t data_r = 0;
    for (uint16_t k=0; k<FFT_N; k++,idx+=4) {
        data_r = data[idx+3] << 8 | data[idx+2];

        vfx_fft_input[k] = (float)data_r;
    }
#else
    int16_t data_l = 0, data_r = 0;
    for (uint16_t k=0; k<FFT_N; k++,idx+=4) {
        data_l = data[idx+1] << 8 | data[idx];
        data_r = data[idx+3] << 8 | data[idx+2];

        vfx_fft_input[k] = (float)((data_l + data_r) / 2);
    }
#endif

    xEventGroupClearBits(user_event_group, VFX_FFT_NULL_BIT);
#endif
}

static void bt_app_alloc_meta_buffer(esp_avrc_ct_cb_param_t *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(param);
    uint8_t *attr_text = (uint8_t *)malloc(rc->meta_rsp.attr_length + 1);

    memcpy(attr_text, rc->meta_rsp.attr_text, rc->meta_rsp.attr_length);

    attr_text[rc->meta_rsp.attr_length] = 0;
    rc->meta_rsp.attr_text = attr_text;
}

void bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        bt_app_alloc_meta_buffer(param);
        /* fall through */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    }
    default:
        break;
    }
}

void bt_app_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_tg_evt, event, param, sizeof(esp_avrc_tg_cb_param_t), NULL);
        break;
    default:
        break;
    }
}

static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)(p_param);
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        ESP_LOGI(BT_A2D_TAG, "A2DP connection state: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 s_a2d_conn_state_str[a2d->conn_stat.state],
                 a2d->conn_stat.remote_bda[0], a2d->conn_stat.remote_bda[1],
                 a2d->conn_stat.remote_bda[2], a2d->conn_stat.remote_bda[3],
                 a2d->conn_stat.remote_bda[4], a2d->conn_stat.remote_bda[5]);

        EventBits_t uxBits = xEventGroupGetBits(user_event_group);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            memset(&a2d_remote_bda, 0x00, sizeof(esp_bd_addr_t));

#ifdef CONFIG_ENABLE_AUDIO_PROMPT
            audio_player_play_file(1);
#endif

            if (!(uxBits & OS_PWR_SLEEP_BIT)) {
                if (!(uxBits & BT_A2DP_IDLE_BIT)) {
#ifdef CONFIG_ENABLE_VFX
                    if (!(uxBits & AUDIO_INPUT_RUN_BIT) && (uxBits & AUDIO_INPUT_FFT_BIT)) {
                        memset(vfx_fft_input, 0x00, sizeof(vfx_fft_input));
                        xEventGroupClearBits(user_event_group, VFX_FFT_NULL_BIT);
                    }
#endif
                }
                if (!(uxBits & OS_PWR_RESTART_BIT)) {
#ifdef CONFIG_ENABLE_LED
                    led_set_mode(3);
#endif
                }
            }

            xEventGroupSetBits(user_event_group, BT_A2DP_IDLE_BIT);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            xEventGroupClearBits(user_event_group, BT_A2DP_IDLE_BIT);
            xEventGroupSetBits(user_event_group, BT_OTA_LOCKED_BIT);

            memcpy(&a2d_remote_bda, a2d->conn_stat.remote_bda, sizeof(esp_bd_addr_t));

            if (memcmp(&last_remote_bda, &a2d_remote_bda, sizeof(esp_bd_addr_t)) != 0) {
                memcpy(&last_remote_bda, &a2d_remote_bda, sizeof(esp_bd_addr_t));
                app_setenv("LAST_REMOTE_BDA", &last_remote_bda, sizeof(esp_bd_addr_t));
            }

#ifdef CONFIG_ENABLE_AUDIO_PROMPT
            audio_player_play_file(0);
#endif
#ifdef CONFIG_ENABLE_LED
            led_set_mode(2);
#endif
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        ESP_LOGI(BT_A2D_TAG, "A2DP audio state: %s", s_a2d_audio_state_str[a2d->audio_stat.state]);
#ifdef CONFIG_ENABLE_LED
        if (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            led_set_mode(1);
        } else {
            led_set_mode(2);
        }
#endif
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        ESP_LOGI(BT_A2D_TAG, "A2DP audio stream configuration, codec type %d", a2d->audio_cfg.mcc.type);
        // for now only SBC stream is supported
        if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            sample_rate = 16000;

            char oct0 = a2d->audio_cfg.mcc.cie.sbc[0];

            if (oct0 & (0x01 << 6)) {
                sample_rate = 32000;
            } else if (oct0 & (0x01 << 5)) {
                sample_rate = 44100;
            } else if (oct0 & (0x01 << 4)) {
                sample_rate = 48000;
            }

            ESP_LOGI(BT_A2D_TAG, "configure audio player %x-%x-%x-%x",
                     a2d->audio_cfg.mcc.cie.sbc[0],
                     a2d->audio_cfg.mcc.cie.sbc[1],
                     a2d->audio_cfg.mcc.cie.sbc[2],
                     a2d->audio_cfg.mcc.cie.sbc[3]);
            ESP_LOGI(BT_A2D_TAG, "audio player configured, sample rate=%d", sample_rate);
        }

        break;
    }
    default:
        ESP_LOGE(BT_A2D_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_av_new_track(void)
{
    // request metadata
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_GENRE;
    esp_avrc_ct_send_metadata_cmd(APP_RC_CT_TL_GET_META_DATA, attr_mask);
    // register notification if peer support the event_id
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_TRACK_CHANGE, ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

static void bt_av_play_status_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_PLAYBACK_CHANGE, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
}

static void bt_av_play_pos_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_PLAY_POS_CHANGED)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_PLAY_POS_CHANGE, ESP_AVRC_RN_PLAY_POS_CHANGED, 10);
    }
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    case ESP_AVRC_RN_TRACK_CHANGE:
        ESP_LOGI(BT_RC_RN_TAG, "Track changed");
        bt_av_new_track();
        break;
    case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
        ESP_LOGI(BT_RC_RN_TAG, "Play status changed: 0x%x", event_parameter->playback);
        bt_av_play_status_changed();
        break;
    case ESP_AVRC_RN_PLAY_POS_CHANGED:
        ESP_LOGI(BT_RC_RN_TAG, "Play position changed: %d-ms", event_parameter->play_pos);
        bt_av_play_pos_changed();
        break;
    }
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected,
                 rc->conn_stat.remote_bda[0], rc->conn_stat.remote_bda[1],
                 rc->conn_stat.remote_bda[2], rc->conn_stat.remote_bda[3],
                 rc->conn_stat.remote_bda[4], rc->conn_stat.remote_bda[5]);

        if (rc->conn_stat.connected) {
            // get remote supported event_ids of peer AVRCP Target
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
        } else {
            // clear peer notification capability record
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough rsp: key_code 0x%x, key_state %d", rc->psth_rsp.key_code, rc->psth_rsp.key_state);
        break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC metadata rsp: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
        free(rc->meta_rsp.attr_text);
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %x, TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        bt_av_new_track();
        bt_av_play_status_changed();
        bt_av_play_pos_changed();
        break;
    }
    default:
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)(p_param);
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected,
                 rc->conn_stat.remote_bda[0], rc->conn_stat.remote_bda[1],
                 rc->conn_stat.remote_bda[2], rc->conn_stat.remote_bda[3],
                 rc->conn_stat.remote_bda[4], rc->conn_stat.remote_bda[5]);
        break;
    }
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC passthrough cmd: key_code 0x%x, key_state %d", rc->psth_cmd.key_code, rc->psth_cmd.key_state);
        break;
    }
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC set absolute volume: %d%%", (int)rc->set_abs_vol.volume * 100 / 0x7f);
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC register event notification: %d, param: 0x%x", rc->reg_ntf.event_id, rc->reg_ntf.event_parameter);
        break;
    }
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT: {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC remote features %x, CT features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.ct_feat_flag);
        break;
    }
    default:
        ESP_LOGE(BT_RC_TG_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

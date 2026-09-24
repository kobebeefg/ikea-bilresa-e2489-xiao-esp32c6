// Observe stack radio callbacks without modifying packets or radio settings.
#include "radio_diagnostics.h"
#include "ezbee/platform/radio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "touchlink_decode.h"
#include <inttypes.h>
#include <string.h>

typedef struct {
    bool tx;
    unsigned error;
    uint8_t channel, length, mac_type;
    int rssi, command;
    uint64_t source;
    uint8_t delivery, zcl_control, scan_length;
    uint8_t scan[39]; // Only public Scan Request/Response fields, never join payloads.
} frame_event_t;
static QueueHandle_t events;
static unsigned rx_count, tx_count, dropped;

static void observe(ezb_radio_frame_t *frame, ezb_err_t error, bool tx)
{
    if (tx) __atomic_fetch_add(&tx_count, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&rx_count, 1, __ATOMIC_RELAXED);
    if (!events || !frame || !frame->psdu || frame->length < 3) return;
    frame_event_t event = {.tx=tx, .error=(unsigned)error, .channel=frame->channel,
        .length=frame->length, .mac_type=frame->psdu[0]&7,
        .rssi=tx ? 127 : frame->info.rx.rssi, .command=-1};
    tl_decoded_t decoded;
    if (error==0 && tl_decode(frame->psdu, frame->length, &decoded)) {
        event.command=decoded.command; event.source=decoded.source;
        event.delivery=decoded.delivery; event.zcl_control=decoded.zcl_control;
        if (decoded.command<=1 && !(decoded.zcl_control&4)) {
            event.scan_length=decoded.size>sizeof(event.scan) ? sizeof(event.scan) : decoded.size;
            memcpy(event.scan, decoded.payload, event.scan_length);
        }
    }
    // Queue Touchlink and MAC command metadata only; total counters cover all frames.
    if (event.command < 0 && event.mac_type != 3) return;
    BaseType_t ok;
    if (xPortInIsrContext()) {
        BaseType_t wake = pdFALSE;
        ok=xQueueSendFromISR(events, &event, &wake);
        if (wake) portYIELD_FROM_ISR();
    } else ok=xQueueSend(events, &event, 0);
    if (ok != pdTRUE) __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
}

void __real_ezb_plat_radio_receive_done(ezb_radio_frame_t *, ezb_err_t);
void __wrap_ezb_plat_radio_receive_done(ezb_radio_frame_t *frame, ezb_err_t error)
{
    observe(frame, error, false);
    __real_ezb_plat_radio_receive_done(frame, error);
}
void __real_ezb_plat_radio_transmit_done(ezb_radio_frame_t *, ezb_radio_frame_t *, ezb_err_t);
void __wrap_ezb_plat_radio_transmit_done(ezb_radio_frame_t *frame, ezb_radio_frame_t *ack, ezb_err_t error)
{
    observe(frame, error, true);
    __real_ezb_plat_radio_transmit_done(frame, ack, error);
}

static void radio_log_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        frame_event_t e;
        if (xQueueReceive(events, &e, pdMS_TO_TICKS(500)) == pdTRUE) {
            ESP_LOGI("RADIO", "%s channel=%u rssi=%d length=%u mac_type=%u touchlink_cmd=%d result=0x%x",
                e.tx?"TX_DONE":"RX", e.channel, e.rssi, e.length, e.mac_type, e.command, e.error);
            if (e.command>=0) ESP_LOGI("RADIO", "TL_HEADER source=0x%016" PRIx64 " delivery=%u zcl=0x%02x", e.source, e.delivery, e.zcl_control);
            const uint8_t *p=e.scan;
            if (e.command==0 && e.scan_length>=6) {
                ESP_LOGI("RADIO", "SCAN_REQUEST transaction=0x%08" PRIx32 " zigbee_info=0x%02x touchlink_info=0x%02x", tl_u32(p), p[4], p[5]);
            } else if (e.command==1 && e.scan_length>=29) {
                ESP_LOGI("RADIO", "SCAN_RESPONSE transaction=0x%08" PRIx32 " key_mask=0x%04x zigbee_info=0x%02x touchlink_info=0x%02x rssi_correction=%u channel=%u pan=0x%04x short=0x%04x subdevices=%u groups=%u", tl_u32(p), tl_u16(p+7), p[5], p[6], p[4], p[22], tl_u16(p+23), tl_u16(p+25), p[27], p[28]);
                if (p[27]==1 && e.scan_length>=36) ESP_LOGI("RADIO", "SCAN_ENDPOINT endpoint=%u profile=0x%04x device=0x%04x version=%u groups=%u", p[29], tl_u16(p+30), tl_u16(p+32), p[34], p[35]);
            }
        }
        if (xTaskGetTickCount()-last >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI("RADIO", "STATS rx_callbacks=%u tx_callbacks=%u dropped_logs=%u",
                __atomic_load_n(&rx_count, __ATOMIC_RELAXED),
                __atomic_load_n(&tx_count, __ATOMIC_RELAXED),
                __atomic_load_n(&dropped, __ATOMIC_RELAXED));
            last=xTaskGetTickCount();
        }
    }
}

void radio_diagnostics_init(void)
{
    // Exercise the actual decoder on a broadcast scan, including every truncation.
    uint8_t fixture[]={0x01,0xc8,1,0xff,0xff,0xff,0xff,0xff,0xff,
        1,2,3,4,5,6,7,8,0x0b,0,0x0b,0,0x10,0x5e,0xc0,0x11,1,0,
        1,2,3,4,4,0x12,0,0};
    tl_decoded_t check;
    configASSERT(tl_decode(fixture,sizeof(fixture),&check));
    configASSERT(check.command==0 && check.delivery==2 && check.size==6 && tl_u32(check.payload)==0x04030201);
    for (size_t n=0;n<29;n++) configASSERT(!tl_decode(fixture,n,&check));
    fixture[19]=3;
    configASSERT(tl_decode(fixture,sizeof(fixture),&check) && check.delivery==0);
    fixture[0]|=8;
    configASSERT(!tl_decode(fixture,sizeof(fixture),&check));
    ESP_LOGI("RADIO", "Decoder self-test passed: broadcast/unicast/truncated/secured frames");
    events=xQueueCreate(32, sizeof(frame_event_t));
    configASSERT(events);
    BaseType_t ok=xTaskCreate(radio_log_task, "radio_log", 3072, NULL, 2, NULL);
    configASSERT(ok == pdPASS);
    ESP_LOGI("RADIO", "Radio callback observers installed; channel filtering is unchanged");
}

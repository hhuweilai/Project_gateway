/*
 * mes_handler.c -- MES event handler implementation
 */

#include "mes_handler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void time_str(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t)
        strftime(buf, size, "%Y-%m-%d %H:%M:%S", t);
    else
        snprintf(buf, size, "unknown");
}

int mes_build_defect(uint8_t *value, uint16_t *len, size_t max_len,
                     int station, const char *defect_type)
{
    char ts[32]; time_str(ts, sizeof(ts));
    int n = snprintf((char *)value, max_len,
        "{\"event\":\"defect\",\"station\":%d,\"type\":\"%s\",\"ts\":\"%s\"}",
        station, defect_type, ts);
    if (n < 0 || (size_t)n >= max_len) return -1;
    *len = (uint16_t)n;
    return 0;
}

int mes_build_heartbeat(uint8_t *value, uint16_t *len, size_t max_len,
                        uint32_t uptime_sec, uint32_t frames_processed)
{
    char ts[32]; time_str(ts, sizeof(ts));
    int n = snprintf((char *)value, max_len,
        "{\"event\":\"heartbeat\",\"uptime\":%u,\"frames\":%u,\"ts\":\"%s\"}",
        uptime_sec, frames_processed, ts);
    if (n < 0 || (size_t)n >= max_len) return -1;
    *len = (uint16_t)n;
    return 0;
}

int mes_build_startup(uint8_t *value, uint16_t *len, size_t max_len)
{
    char ts[32]; time_str(ts, sizeof(ts));
    int n = snprintf((char *)value, max_len,
        "{\"event\":\"startup\",\"device\":\"iMX6ULL-Gateway\",\"ts\":\"%s\"}",
        ts);
    if (n < 0 || (size_t)n >= max_len) return -1;
    *len = (uint16_t)n;
    return 0;
}

size_t mes_pack_defect(uint8_t *frame, size_t frame_max,
                       int station, const char *defect_type)
{
    uint8_t value[MES_JSON_MAX];
    uint16_t len = 0;
    if (mes_build_defect(value, &len, sizeof(value), station, defect_type) < 0)
        return 0;
    return tlv_pack(TLV_MES_EVENT, value, len, frame, frame_max);
}

size_t mes_pack_heartbeat(uint8_t *frame, size_t frame_max,
                          uint32_t uptime_sec, uint32_t frames_processed)
{
    uint8_t value[MES_JSON_MAX];
    uint16_t len = 0;
    if (mes_build_heartbeat(value, &len, sizeof(value), uptime_sec, frames_processed) < 0)
        return 0;
    return tlv_pack(TLV_HEARTBEAT_REQ, value, len, frame, frame_max);
}

size_t mes_pack_startup(uint8_t *frame, size_t frame_max)
{
    uint8_t value[MES_JSON_MAX];
    uint16_t len = 0;
    if (mes_build_startup(value, &len, sizeof(value)) < 0)
        return 0;
    return tlv_pack(TLV_MES_EVENT, value, len, frame, frame_max);
}

size_t mes_pack_ack(uint8_t *frame, size_t frame_max, uint8_t ack_tag)
{
    uint8_t ack_val[16];
    int n = snprintf((char *)ack_val, sizeof(ack_val), "{\"ack\":%u}", ack_tag);
    if (n < 0) return 0;
    return tlv_pack(TLV_MES_ACK, ack_val, (uint16_t)n, frame, frame_max);
}

size_t mes_pack_ai_result(uint8_t *frame, size_t frame_max,
                          int has_defect, const char *defect_type, float confidence,
                          int x, int y, int w, int h)
{
    uint8_t value[MES_JSON_MAX];
    uint16_t len = 0;
    int n = snprintf((char *)value, sizeof(value),
        "{\"defect\":%d,\"type\":\"%s\",\"conf\":%.2f,\"bbox\":[%d,%d,%d,%d]}",
        has_defect, defect_type, confidence, x, y, w, h);
    if (n < 0 || (size_t)n >= sizeof(value)) return 0;
    len = (uint16_t)n;
    return tlv_pack(TLV_AI_RESULT, value, len, frame, frame_max);
}

size_t mes_pack_ai_ready(uint8_t *frame, size_t frame_max)
{
    const char *val = "{\"status\":\"ready\"}";
    return tlv_pack(TLV_AI_READY, (const uint8_t *)val, (uint16_t)strlen(val), frame, frame_max);
}

size_t mes_pack_ai_cmd(uint8_t *frame, size_t frame_max, int enable)
{
    uint8_t val[32];
    int n = snprintf((char *)val, sizeof(val), "{\"cmd\":\"%s\"}", enable ? "start" : "stop");
    if (n < 0) return 0;
    return tlv_pack(TLV_AI_CMD, val, (uint16_t)n, frame, frame_max);
}

int mes_parse_ai_result(const uint8_t *value, uint16_t len,
                        int *has_defect, char *defect_type, size_t type_size,
                        float *confidence)
{
    char buf[MES_JSON_MAX + 1];
    size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, value, copy);
    buf[copy] = '\0';

    char *p;
    p = strstr(buf, "\"defect\":");
    if (p) *has_defect = atoi(p + 9);
    else  *has_defect = 0;

    p = strstr(buf, "\"type\":\"");
    if (p && defect_type) {
        p += 8;
        char *end = strchr(p, '"');
        if (end) {
            size_t sz = (size_t)(end - p) < type_size - 1 ? (size_t)(end - p) : type_size - 1;
            memcpy(defect_type, p, sz);
            defect_type[sz] = '\0';
        }
    }

    p = strstr(buf, "\"conf\":");
    if (p && confidence) *confidence = (float)atof(p + 7);

    return 0;
}

void mes_print_event(uint8_t tag, const uint8_t *value, uint16_t len)
{
    char buf[MES_JSON_MAX + 1];
    size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, value, copy);
    buf[copy] = '\0';

    const char *tag_name = "UNKNOWN";
    switch (tag) {
    case TLV_HEARTBEAT_REQ: tag_name = "HEARTBEAT_REQ"; break;
    case TLV_HEARTBEAT_RSP: tag_name = "HEARTBEAT_RSP"; break;
    case TLV_MES_EVENT:     tag_name = "MES_EVENT";     break;
    case TLV_MES_ACK:       tag_name = "MES_ACK";       break;
    case TLV_FRAME_DATA:    tag_name = "FRAME_DATA";    break;
    case TLV_FRAME_ACK:     tag_name = "FRAME_ACK";     break;
    case TLV_FRAME_START:   tag_name = "FRAME_START";   break;
    case TLV_AI_RESULT:     tag_name = "AI_RESULT";     break;
    case TLV_AI_READY:      tag_name = "AI_READY";      break;
    case TLV_AI_CMD:        tag_name = "AI_CMD";        break;
    }

    printf("[MES] %s | %s\n", tag_name, buf);
}
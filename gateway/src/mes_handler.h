/*
 * mes_handler.h 鈥?MES 浜嬩欢澶勭悊
 */

#ifndef MES_HANDLER_H
#define MES_HANDLER_H

#include "tlv_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MES_JSON_MAX  512

/* 鏋勫缓浜嬩欢 鈫?value 濉厖涓?JSON */
int mes_build_defect(uint8_t *value, uint16_t *len, size_t max_len,
                     int station, const char *defect_type);
int mes_build_heartbeat(uint8_t *value, uint16_t *len, size_t max_len,
                        uint32_t uptime_sec, uint32_t frames_processed);
int mes_build_startup(uint8_t *value, uint16_t *len, size_t max_len);

/* 鎵撳寘瀹屾暣 TLV 甯?*/
size_t mes_pack_defect(uint8_t *frame, size_t frame_max,
                       int station, const char *defect_type);
size_t mes_pack_heartbeat(uint8_t *frame, size_t frame_max,
                          uint32_t uptime_sec, uint32_t frames_processed);
size_t mes_pack_startup(uint8_t *frame, size_t frame_max);
size_t mes_pack_ack(uint8_t *frame, size_t frame_max, uint8_t ack_tag);

/* AI 结果构建 (PC→网关) */
int mes_build_ai_result(uint8_t *value, uint16_t *len, size_t max_len,
                        int has_defect, const char *defect_type, float confidence,
                        int x, int y, int w, int h);

size_t mes_pack_ai_result(uint8_t *frame, size_t frame_max,
                          int has_defect, const char *defect_type, float confidence,
                          int x, int y, int w, int h);
size_t mes_pack_ai_ready(uint8_t *frame, size_t frame_max);
size_t mes_pack_ai_cmd(uint8_t *frame, size_t frame_max, int enable);

/* AI 结果解析 (网关端接收) */
int mes_parse_ai_result(const uint8_t *value, uint16_t len,
                        int *has_defect, char *defect_type, size_t type_size,
                        float *confidence);

/* 瑙ｆ瀽鎵撳嵃 */
void mes_print_event(uint8_t tag, const uint8_t *value, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
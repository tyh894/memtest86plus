#ifndef _DONGLE_H
#define _DONGLE_H

#include <stdint.h>
#include <stdbool.h>

// ==========================================
// 加密狗功能宏配置
// 1: 开启加密狗验证 (编译出的程序必须接加密狗)
// 0: 关闭加密狗验证 (编译出的程序不需要接加密狗)
// ==========================================
#define REQUIRE_DONGLE 0

#pragma pack(push, 1)
typedef struct {      
    uint8_t header[6];    // Header: "HEROJE"
    uint32_t timestamp;   // Timestamp: UNIX timestamp(s)
    uint8_t uid[12];      // UID: N32L403 96-bit
    int16_t temperature;  // Temperature (0.1 C)
    uint8_t check;        // Checksum
    uint8_t tail[6];      // Tail: "HEROJE"
} data_packet_t;
#pragma pack(pop)

void dongle_init(void);
void dongle_send_time_packet(void);
void dongle_feed_rx_byte(uint8_t c);
bool dongle_is_authorized(void);
void dongle_reset_auth(void);

#endif /* _DONGLE_H */

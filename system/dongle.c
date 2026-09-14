#include "dongle.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "display.h"
#include "aes.h"
#include "tsc.h"


// BCD to Decimal
static uint8_t bcd_to_dec(uint8_t val) {
    return ((val / 16) * 10) + (val % 16);
}

// Read RTC Register
static uint8_t read_rtc(uint8_t reg) {
    __outb(reg, 0x70);
    return __inb(0x71);
}

// Days per month (non-leap year)
static const uint8_t month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// Get current UNIX timestamp (UTC)
static uint32_t get_unix_timestamp(void) {
    // Wait for RTC Update-In-Progress flag to clear
    while (read_rtc(0x0A) & 0x80); 
    
    uint8_t sec = bcd_to_dec(read_rtc(0x00));
    uint8_t min = bcd_to_dec(read_rtc(0x02));
    uint8_t hour = bcd_to_dec(read_rtc(0x04));
    uint8_t day = bcd_to_dec(read_rtc(0x07));
    uint8_t month = bcd_to_dec(read_rtc(0x08));
    uint8_t year_lsb = bcd_to_dec(read_rtc(0x09));
    
    // CMOS RTC default base year is 2000
    uint16_t year = 2000 + year_lsb; 

    // Calculate days since 1970
    uint32_t days = 0;
    for (uint16_t y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (uint8_t m = 1; m < month; m++) {
        days += month_days[m - 1];
        if (m == 2 && is_leap_year(year)) {
            days += 1;
        }
    }
    days += day - 1;

    // Convert to seconds
    uint32_t timestamp = days * 86400 + hour * 3600 + min * 60 + sec;
    return timestamp;
}

void dongle_init(void) {
#if REQUIRE_DONGLE
    // Reserved for future crypto initialization
#endif
}

static uint8_t rx_buf[128];
static uint16_t rx_sta = 0;

static uint8_t current_uid[12] = {0};
static bool has_uid = false;
static uint32_t last_sent_timestamp = 0;
static uint64_t last_auth_tick = 0;
static bool is_authorized = false;

extern uint32_t clks_per_msec;

void dongle_send_time_packet(void) {
#if REQUIRE_DONGLE
    data_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    memcpy(pkt.header, "HEROJE", 6);
    memcpy(pkt.tail, "HEROJE", 6);

    uint32_t ts = get_unix_timestamp();
    // uint32_t ts = 0x6A510DF3;
    last_sent_timestamp = ts;

    // Use HEROJE as initial key, or UID if we have it
    uint8_t aes_key[16] = {0};
    if (has_uid) {
        memcpy(aes_key, current_uid, 12);
    } else {
        memcpy(aes_key, "HEROJE", 6);
    }

    uint8_t block[16] = {0};
    block[0] = (ts) & 0xFF;
    block[1] = (ts >> 8) & 0xFF;
    block[2] = (ts >> 16) & 0xFF;
    block[3] = (ts >> 24) & 0xFF;

    aes_context ctx;
    aes_set_key(&ctx, aes_key, 128);
    aes_encrypt(&ctx, block, block);

    memcpy(&pkt.timestamp, block, 4);
    memcpy(pkt.uid, block + 4, 12);

    serial_send_bytes((const uint8_t*)&pkt, sizeof(pkt));
#endif
}

void dongle_feed_rx_byte(uint8_t c) {
#if REQUIRE_DONGLE
    if (rx_sta < sizeof(rx_buf)) {
        rx_buf[rx_sta++] = c;
    }

    if (rx_sta >= sizeof(data_packet_t)) {
        uint8_t* valid_pkt_start = NULL;
        uint16_t match_idx = 0;
        bool found_header = false;
        
        for (uint16_t i = 0; i <= rx_sta - sizeof(data_packet_t); i++) {
            if (rx_buf[i] == 'H' && rx_buf[i+1] == 'E' && rx_buf[i+2] == 'R' && 
                rx_buf[i+3] == 'O' && rx_buf[i+4] == 'J' && rx_buf[i+5] == 'E') {
                
                found_header = true;
                // ????????????
                if (memcmp(&rx_buf[i + sizeof(data_packet_t) - 6], "HEROJE", 6) == 0) {
                    valid_pkt_start = &rx_buf[i];
                    match_idx = i;
                    break;
                }
            }
        }

        if (valid_pkt_start != NULL) {
            data_packet_t* pkt = (data_packet_t*)valid_pkt_start;

            if (pkt->check == 1) {
                // Decrypt
                uint8_t block[16] = {0};
                memcpy(block, &pkt->timestamp, 4);
                memcpy(block + 4, pkt->uid, 12);

                uint8_t aes_key[16] = {0};
                // ??????? timestamp ??????????
                aes_key[0] = (last_sent_timestamp) & 0xFF;
                aes_key[1] = (last_sent_timestamp >> 8) & 0xFF;
                aes_key[2] = (last_sent_timestamp >> 16) & 0xFF;
                aes_key[3] = (last_sent_timestamp >> 24) & 0xFF;

                aes_context ctx;
                aes_set_key(&ctx, aes_key, 128);
                aes_decrypt(&ctx, block, block);

                uint32_t dec_ts = ((uint32_t)block[3] << 24) | ((uint32_t)block[2] << 16) | ((uint32_t)block[1] << 8) | ((uint32_t)block[0]);

                // ?????? dec_ts (??? Need Dongle ??????)
                // printf(17, 28, "dec_ts: 0x%08x      ", dec_ts);

                if (dec_ts == last_sent_timestamp) {
                    // printf(17, 32, "dec_ts == last_sent_timestamp: 0x%08x      ", dec_ts);
                    memcpy(current_uid, block + 4, 12);
                    has_uid = true;
                    is_authorized = true;
                    
                    last_auth_tick = get_tsc();
                    
                    int int_part = pkt->temperature / 10;
                    int frac_part = pkt->temperature % 10;
                    if (frac_part < 0) frac_part = -frac_part;
                    printf(0, 69, "Temp: %u.%u C      ", int_part, frac_part);
                    
                    // serial_send_bytes((const uint8_t*)"\r\n[PC] Decrypt Success! UID Saved!\r\n", 36);
                } else {
                    // printf(17, 32, "dec_ts != last_sent_timestamp: 0x%08x      ", dec_ts);
                    // ???????????? has_uid?????????????
                    // ?? 5 ????? (dongle_is_authorized) ???????
                }
            }
            
            // ????????????????????????
            uint16_t consume_len = match_idx + sizeof(data_packet_t);
            if (rx_sta > consume_len) {
                memmove(rx_buf, rx_buf + consume_len, rx_sta - consume_len);
                rx_sta -= consume_len;
            } else {
                rx_sta = 0;
            }
        } else {
            // ??????128???????????????????
            if (rx_sta >= (sizeof(rx_buf) - 16)) {
                rx_sta = 0;
            }
            // ?????????????????????????????
            if (!found_header && rx_sta >= 64) {
                rx_sta = 0;
            }
        }
    }
#endif
}

bool dongle_is_authorized(void) {
#if !REQUIRE_DONGLE
    return true;
#else
    if (!is_authorized) return false;
    
    uint64_t current_tick = get_tsc();
    
    // If no valid heartbeat for 10 seconds, revoke authorization
    if ((current_tick - last_auth_tick) > (10000ULL * clks_per_msec)) {
        is_authorized = false;
        has_uid = false;
        return false;
    }
    
    return true;
#endif
}

void dongle_reset_auth(void) {
#if REQUIRE_DONGLE
    is_authorized = false;
    has_uid = false;
    last_sent_timestamp = 0;
    rx_sta = 0;
    memset(current_uid, 0, sizeof(current_uid));
#endif
}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2020-2022 Martin Whitaker.
// Copyright (C) 2004-2025 Sam Demeulemeester.

#include <stdbool.h>
#include <stdint.h>

#include "cpuid.h"
#include "cpulocal.h"
#include "cpuinfo.h"
#include "hwctrl.h"
#include "i2c_x86.h"
#include "io.h"
#include "keyboard.h"
#include "memctrl.h"
#include "serial.h"
#include "pmem.h"
#include "smbios.h"
#include "smp.h"
#include "spd.h"
#include "temperature.h"
#include "tsc.h"

#include "barrier.h"
#include "spinlock.h"

#include "config.h"
#include "error.h"
#include "lang.h"
#include "build_version.h"

#include "tests.h"
#include "test.h"

#include "display.h"
#include "smp.h"
#include "dongle.h"
//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------

#define POP_STAT_R       12
#define POP_STAT_C       18

#define POP_STAT_W       44
#define POP_STAT_H       11

#define POP_STAT_LAST_R  (POP_STAT_R + POP_STAT_H - 1)
#define POP_STAT_LAST_C  (POP_STAT_C + POP_STAT_W - 1)

#define POP_STATUS_REGION  POP_STAT_R, POP_STAT_C, POP_STAT_LAST_R, POP_STAT_LAST_C

// About (licensing) pop-up window. Same size as the config menu window.

#define ABOUT_R         3
#define ABOUT_C         21

#define ABOUT_W         45
#define ABOUT_H         18

#define ABOUT_LAST_R    (ABOUT_R + ABOUT_H - 1)
#define ABOUT_LAST_C    (ABOUT_C + ABOUT_W - 1)

#define ABOUT_REGION    ABOUT_R, ABOUT_C, ABOUT_LAST_R, ABOUT_LAST_C

// Scrolling ad banner (bottom screen row).

#define ROW_MARQUEE     (SCREEN_HEIGHT - 1)

#define MARQUEE_PERIOD  150     // milliseconds

#define SPINNER_PERIOD  100     // milliseconds

#define NUM_SPIN_STATES 4

static const char spin_state[NUM_SPIN_STATES] = { '|', '/', '-', '\\' };

static const char cpu_mode_str[3][4] = { "PAR", "SEQ", "RR " };

//------------------------------------------------------------------------------
// Private Variables
//------------------------------------------------------------------------------

static bool scroll_lock = false;
static bool scroll_wait = false;

static int spin_idx = 0;        // current spinner position

static int pass_ticks = 0;      // current value (ticks_per_pass is final value)
static int test_ticks = 0;      // current value (ticks_per_test is final value)

static int pass_bar_length = 0; // currently displayed length
static int test_bar_length = 0; // currently displayed length
static int test_bar_colour = 2; // colour for test progress bar

static uint64_t next_spin_time = 0; // TSC time stamp
static uint64_t test_next_spin_time = 0;
static int prev_sec = -1;               // previous second
static bool timed_update_done = false;  // update cycle status

bool big_status_displayed = false;
static shadow_char_t popup_status_save_buffer[POP_STAT_W * POP_STAT_H];
static shadow_char_t about_save_buffer[ABOUT_W * ABOUT_H];

// Scrolling ad banner text. Padded with spaces so the wrap-around has a gap.
static const char marquee_text[] =
    "                                                                                "
    "www.herotest.cn 存储颗粒稳定性及频率测试软件，DDR5服务器内存专用测试平台，"
    "SLT内存模组测试机台，受托测试服务。欢迎定制测试软件及机台。版权保护，请勿盗版。          ";

static int marquee_offset = 0;  // horizontal scroll offset in columns

//------------------------------------------------------------------------------
// Variables
//------------------------------------------------------------------------------

int scroll_message_row;

uint64_t run_start_time = 0; // TSC time stamp

int max_cpu_temp = TEMP_INVALID;

display_mode_t display_mode = DISPLAY_MODE_NA;

screen_palette_t palette = {BLUE, WHITE, WHITE, BLACK, WHITE, BLUE, BLACK};

//------------------------------------------------------------------------------
// Private Functions
//------------------------------------------------------------------------------

static void marquee_draw(void);

static void set_screen_palette(screen_palette_t *mt_palette)
{
    if (dark_mode) {
        *mt_palette = (screen_palette_t){
            .background        = BLACK,
            .foreground        = WHITE,
            .title_background  = WHITE,
            .title_foreground  = BLACK,
            .footer_background = WHITE,
            .footer_foreground = BLACK,
            .popup_background  = WHITE
        };
    } else {
        *mt_palette = (screen_palette_t){
            .background        = BLUE,
            .foreground        = WHITE,
            .title_background  = WHITE,
            .title_foreground  = BLACK,
            .footer_background = WHITE,
            .footer_foreground = BLUE,
            .popup_background  = BLACK
        };
    }
}

//------------------------------------------------------------------------------
// Test mode selection (F3 flash / F4 standard / F5 burn-in)
//------------------------------------------------------------------------------

extern int test_sequence[NUM_TEST_PATTERNS];

static int test_mode = 3;   // 3 = F3 flash, 4 = F4 standard, 5 = F5 burn-in

// Redraw the footer with the currently selected mode highlighted in red.
static void footer_draw(void)
{
    set_foreground_colour(palette.footer_foreground);
    set_background_colour(palette.footer_background);
    clear_screen_region(ROW_FOOTER, 0, ROW_FOOTER, SCREEN_WIDTH - 1);
    prints(ROW_FOOTER, 0, " <ESC> 退出 <F1> 配置 ");
    set_foreground_colour(test_mode == 3 ? BLUE : palette.footer_foreground);
    prints(ROW_FOOTER, 22, "<F3> 闪测 ");
    set_foreground_colour(test_mode == 4 ? BLUE : palette.footer_foreground);
    prints(ROW_FOOTER, 32, "<F4> 标准 ");
    set_foreground_colour(test_mode == 5 ? BLUE : palette.footer_foreground);
    prints(ROW_FOOTER, 42, "<F5> 老化 ");
    if (scroll_lock) {
        printc(ROW_FOOTER, SCREEN_WIDTH - 1, '*');
    }
    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);
}

// Apply a test mode: F3 = tests 0-4, F4/F5 = all tests, F5 = 999 passes.
static void set_test_mode(int mode)
{
    test_mode = mode;

    bool enable_all = (mode != 3);
    for (int i = 0; i < NUM_TEST_PATTERNS; i++) {
        test_list[i].enabled = enable_all || (i <= 4);
    }

    // Rebuild the test sequence from the enabled flags and terminate it
    // with NUM_TEST_PATTERNS so the main loop stops at the end of a pass.
    int n = 0;
    for (int i = 0; i < NUM_TEST_PATTERNS; i++) {
        if (test_list[i].enabled) {
            test_sequence[n++] = i;
        }
    }
    for (int i = n; i < NUM_TEST_PATTERNS; i++) {
        test_sequence[i] = NUM_TEST_PATTERNS;
    }

    // F5 burn-in loops 999 passes; F3/F4 run a single pass (also restores
    // max_pass_num when switching back from F5).
    max_pass_num = (mode == 5) ? 999 : 1;

    footer_draw();
}

//------------------------------------------------------------------------------
// Public Functions
//------------------------------------------------------------------------------

void display_init(void)
{
    cursor_off();

    set_screen_palette(&palette);
    set_background_colour(palette.background);

    clear_screen();

    /* The commented horizontal lines provide visual cue for where and how
     * they will appear on the screen. They are drawn down below using
     * Extended ASCII characters.
     */

    set_foreground_colour(palette.title_foreground);
    set_background_colour(palette.title_background);
    clear_screen_region(0, 0, 0, 27);
    prints(0, 0, lang_cn ? " HEROSYS 内存老化测试 V"MT_VERSION : " HEROSYS BURN-IN TEST V"MT_VERSION);
    set_foreground_colour(RED);
    printc(0, 27, '+');
    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);

    // Ad slot 1: licensing banner on the title row ("创芯界" in red).
    prints(0, 37, "授权于 ");
    set_foreground_colour(RED);
    prints(0, 44, "创芯界");
    set_foreground_colour(palette.foreground);
    prints(0, 50, " 品牌内存产品测试服务");
    prints(1, 0, "时钟/温度: N/A              | 轮次   %");
    prints(2, 0, "L1 缓存:  N/A               | 测试   %");
    prints(3, 0, "L2 缓存:  N/A               | 测试 #");
    prints(4, 0, "L3 缓存:  N/A               | 测试项:");
    prints(5, 0, "内存容量: N/A               | 数据模式:");
//  prints(6, 0, "--------------------------------------------------------------------------------");
    prints(7, 0, "轮次:          错误:                      |时间:           状态:  初始化");
//  prints(9, 0, "--------------------------------------------------------------------------------");

    if (ecc_status.ecc_enabled) {
        prints(7, 16, "错误:       ECC:");
    }

    for (int i = 0; i < 80; i++) {
        print_char(6, i, 0xc4);
        print_char(8, i, 0xc4);
    }
    for (int i = 0; i < 6; i++) {
        print_char(i, 28, 0xb3);
    }
    for (int i = 7; i < 9; i++) {
        print_char(i, 42, 0xb3);
    }

    print_char(6, 28, 0xc1);
    print_char(6, 42, 0xc2);
    print_char(8, 42, 0xc1);

    // Footer with the mode keys; F3 (flash) is selected by default at boot.
    set_test_mode(3);

    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);

    marquee_draw();

    if (cpu_model) {
        display_cpu_model(cpu_model);
    }
#if defined(__aarch64__)
    // Generic timer does not run at CPU clock. Use the PMU instead
    // if (cpu_clk_mhz) {
    //     display_cpu_clk((int)cpu_clk_mhz);//显示CPU信息
    // }
#else
    if (clks_per_msec) {
        display_cpu_clk((int)(clks_per_msec / 1000));
    }
#endif
#if TESTWORD_WIDTH < 64
    if (cpuid_info.flags.lm) {
        display_cpu_addr_mode(" [LM]");
    } else if (cpuid_info.flags.pae) {
        display_cpu_addr_mode("[PAE]");
    }
#endif
    if (l1_cache) {
        display_l1_cache_size(l1_cache);
    }
    if (l2_cache) {
        display_l2_cache_size(l2_cache);
    }
    if (l3_cache) {
        display_l3_cache_size(l3_cache);
    }
    if (l1_cache_speed) {
        display_l1_cache_speed(l1_cache_speed);
    }
    if (l2_cache_speed) {
        display_l2_cache_speed(l2_cache_speed);
    }
    if (l3_cache_speed) {
        display_l3_cache_speed(l3_cache_speed);
    }
    if (ram_speed) {
        display_ram_speed(ram_speed);
    }
    if (num_pm_pages) {
        // Round to nearest MB.
        display_memory_size(1024 * ((num_pm_pages + 128) / 256));
    }

    scroll_message_row = ROW_SCROLL_T;
}

void display_cpu_topology(void)
{
    extern int num_enabled_cpus;
    int num_cpu_sockets = 1;

    // Display Thread Count and Thread Dispatch Mode
    if (smp_enabled) {
        if (cpuid_info.topology.is_hybrid && cpuid_info.topology.ecore_count > 0 && exclude_ecores) {
            display_threading(num_enabled_cpus - cpuid_info.topology.ecore_count, cpu_mode_str[cpu_mode]);
        } else {
            display_threading(num_enabled_cpus, cpu_mode_str[cpu_mode]);
        }
    } else {
        display_threading_disabled();
    }

    // If topology failed, assume topology according to APIC
    if (cpuid_info.topology.core_count <= 0) {

        cpuid_info.topology.core_count = num_enabled_cpus;
        cpuid_info.topology.thread_count = num_enabled_cpus;

        if(cpuid_info.flags.htt && num_enabled_cpus >= 2 && num_enabled_cpus % 2 == 0) {
            cpuid_info.topology.core_count /= 2;
        }
    }

    // Compute number of sockets according to individual CPU core count
    if (num_enabled_cpus > cpuid_info.topology.thread_count &&
        num_enabled_cpus % cpuid_info.topology.thread_count == 0) {
        num_cpu_sockets  = num_enabled_cpus / cpuid_info.topology.thread_count;
    }

    // Display P/E-Core count for Hybrid CPUs.
    if (cpuid_info.topology.is_hybrid) {
        if (cpuid_info.topology.pcore_count > 1) {

            if (cpuid_info.flags.htt && cpuid_info.topology.thread_per_core > 1 &&
                (cpuid_info.topology.thread_count - cpuid_info.topology.ecore_count) == cpuid_info.topology.pcore_count) {
                    cpuid_info.topology.pcore_count /= 2;
            }

            display_cpu_topo_hybrid(cpuid_info.topology.pcore_count,
                                    cpuid_info.topology.ecore_count,
                                    cpuid_info.topology.thread_count);
        } else {
            display_cpu_topo_hybrid_short(cpuid_info.topology.thread_count);
        }
        return;
    }

    // Condensed display for multi-socket motherboard
    if (num_cpu_sockets > 1) {
        display_cpu_topo_multi_socket(num_cpu_sockets,
                                      num_cpu_sockets * cpuid_info.topology.core_count,
                                      num_cpu_sockets * cpuid_info.topology.thread_count);
        return;
    }

    if (cpuid_info.topology.thread_count < 100) {
        display_cpu_topo(cpuid_info.topology.core_count,
                         cpuid_info.topology.thread_count);
    } else {
        display_cpu_topo_short(cpuid_info.topology.core_count,
                               cpuid_info.topology.thread_count);
    }

}

void post_display_init(void)
{
    print_smbios_startup_info();

    if (print_spd_startup_info() == 0) {
        // No SPD data, fall back to SMBIOS Type 17 info.
        print_dmi_memory_info();
    }

    if (imc.freq) {
        // Try to get RAM information from IMC
        display_spec_mode("IMC: ");
        if (imc.type[3] == '5') {
            display_spec_ddr5(imc.freq, imc.type, imc.tCL, imc.tCL_dec, imc.tRCD, imc.tRP, imc.tRAS);
        } else {
            display_spec_ddr(imc.freq, imc.type, imc.tCL, imc.tCL_dec, imc.tRCD, imc.tRP, imc.tRAS);
        }
        display_mode = DISPLAY_MODE_IMC;
    } else if (ram.freq > 0 && ram.tCL > 0) {
        // If not available, grab max memory specs from SPD
        display_spec_mode("RAM: ");
        if (ram.freq <= 166) {
            display_spec_sdr(ram.freq, ram.type, ram.tCL, ram.tRCD, ram.tRP, ram.tRAS);
        } else {
            display_spec_ddr(ram.freq, ram.type, ram.tCL, ram.tCL_dec, ram.tRCD, ram.tRP, ram.tRAS);
        }
        display_mode = DISPLAY_MODE_SPD;
    } else {
        // If nothing available, fallback to "Using Core" Display
        display_mode = DISPLAY_MODE_NA;
    }
}

void display_start_run(void)
{
    if (!enable_trace && !enable_sm) {
        clear_message_area();
    }

    clear_screen_region(7, 49, 7, 57);                      // run time

    if (ecc_status.ecc_enabled) {
        clear_screen_region(7, 7, 7, 9);                    // pass number
        clear_screen_region(7, 22, 7, 31);                  // error count
        clear_screen_region(7, 33, 7, 41);                  // ecc error count
    } else {
        clear_screen_region(7, 7, 7, 9);                    // pass number
        clear_screen_region(7, 22, 7, 41);                  // error count
    }

    display_pass_count(0);
    error_count = 0;
    display_error_count();
    if (clks_per_msec > 0) {
        // If we've measured the CPU speed, we know the TSC is available.
        run_start_time = get_tsc();
        next_spin_time = run_start_time + SPINNER_PERIOD * clks_per_msec;
    }
    display_spinner('-');
    display_status("测试中");

    if (enable_tty){
        // tty_full_redraw();
        // direct_send_string("tty_full_redraw");
    }
}

void display_start_pass(void)
{
    clear_screen_region(1, 39, 1, SCREEN_WIDTH - 1);    // progress bar
    display_pass_percentage(0);
    pass_bar_length = 0;
    pass_ticks = 0;
}

void display_start_test(void)
{
    clear_screen_region(2, 39, 3, SCREEN_WIDTH - 1);    // progress bar, test details
    clear_screen_region(4, 39, 4, SCREEN_WIDTH - 6);    // Avoid erasing paging mode
    clear_screen_region(5, 39, 5, SCREEN_WIDTH - 1);
    clear_screen_region(3, 36, 3, 37);                  // test number
    display_test_percentage(0);
    display_test_number(test_num);
    display_test_description(test_list[test_num].description);
    test_bar_length = 0;
    test_bar_colour = ((get_tsc() >> 8) & 0x7) + 1;
    test_ticks = 0;

#if 0
    uint64_t current_time = get_tsc();
    int secs = (current_time - run_start_time) / (1000 * (uint64_t)clks_per_msec);
    int mins  = secs / 60; secs %= 60;
    int hours = mins / 60; mins %= 60;
    do_trace(0, "T %i: %i:%02i:%02i", test_num, hours, mins, secs);
#endif
}

void display_error_count(void)
{
    if (ecc_status.ecc_enabled) {
        display_err_count_with_ecc(error_count, error_count_cecc);
    } else {
        display_err_count_without_ecc(error_count);
    }
}

void display_temperature(void)
{
    if (enable_temp_cpu) {
        // Display CPU Temperature
        int actual_cpu_temp = get_cpu_temp();

        if (actual_cpu_temp == TEMP_INVALID) {
            if (max_cpu_temp == TEMP_INVALID) {
                enable_temp_cpu = false;
            }
            return;
        }

        if (max_cpu_temp == TEMP_INVALID || max_cpu_temp < actual_cpu_temp) {
            max_cpu_temp = actual_cpu_temp;
        }

        int offset = TEMP_LEN(actual_cpu_temp) + TEMP_LEN(max_cpu_temp) + 1;

        display_cpu_temperature(actual_cpu_temp, max_cpu_temp, offset);
    }

    if (enable_temp_ram) {
        // Display RAM Temperature (DDR5+ Only) - LA64 unsupported yet
        if (dmi_memory_device_type == DMI_DDR5 && !strstr(cpuid_info.vendor_id.str, "Loongson")) {

            for (int i = 0; i < MAX_SPD_SLOT; i++) {

                if (!ram_slot_info[i].isPopulated || !ram_slot_info[i].hasTempSensor)
                    continue;

                int ram_temp = get_ram_temp(i);

                if (ram_temp != TEMP_INVALID) {
                    display_ram_temperature(ram_temp, ram_slot_info[i].display_idx)
                }
            }
        }
    }
}

void display_big_status(bool pass)
{
    if (!enable_big_status || big_status_displayed) {
        return;
    }

    save_screen_region(POP_STATUS_REGION, popup_status_save_buffer);

    set_background_colour(palette.popup_background);
    set_foreground_colour(pass ? GREEN : RED);
    clear_screen_region(POP_STATUS_REGION);

    if (pass) {
        prints(POP_STAT_R+1, POP_STAT_C+5, "######      ##      #####    #####  ");
        prints(POP_STAT_R+2, POP_STAT_C+5, "##   ##    ####    ##   ##  ##   ## ");
        prints(POP_STAT_R+3, POP_STAT_C+5, "##   ##   ##  ##   ##       ##      ");
        prints(POP_STAT_R+4, POP_STAT_C+5, "######   ##    ##   #####    #####  ");
        prints(POP_STAT_R+5, POP_STAT_C+5, "##       ########       ##       ## ");
        prints(POP_STAT_R+6, POP_STAT_C+5, "##       ##    ##  ##   ##  ##   ## ");
        prints(POP_STAT_R+7, POP_STAT_C+5, "##       ##    ##   #####    #####  ");
    } else {
        prints(POP_STAT_R+1, POP_STAT_C+5, "#######     ##      ######   ##     ");
        prints(POP_STAT_R+2, POP_STAT_C+5, "##         ####       ##     ##     ");
        prints(POP_STAT_R+3, POP_STAT_C+5, "##        ##  ##      ##     ##     ");
        prints(POP_STAT_R+4, POP_STAT_C+5, "#####    ##    ##     ##     ##     ");
        prints(POP_STAT_R+5, POP_STAT_C+5, "##       ########     ##     ##     ");
        prints(POP_STAT_R+6, POP_STAT_C+5, "##       ##    ##     ##     ##     ");
        prints(POP_STAT_R+7, POP_STAT_C+5, "##       ##    ##   ######   ###### ");
    }

    prints(POP_STAT_R+8, POP_STAT_C+5, "                                    ");
    prints(POP_STAT_R+9, POP_STAT_C+5, "按任意键关闭此提示 ");

    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);
    big_status_displayed = true;
}

void restore_big_status(void)
{
    if (!big_status_displayed) {
        return;
    }

    restore_screen_region(POP_STATUS_REGION, popup_status_save_buffer);
    big_status_displayed = false;
}

//------------------------------------------------------------------------------
// Ad slot 2: scrolling banner on the bottom screen row
//------------------------------------------------------------------------------

// Minimal UTF-8 decoder (the UI string literals are valid UTF-8).
static int utf8_decode(const char *s, uint32_t *cp)
{
    uint8_t b = (uint8_t)s[0];

    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(b & 0x1F) << 6) | (uint8_t)(s[1] & 0x3F);
        return 2;
    }
    if ((b & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint8_t)(s[2] & 0x3F);
        return 3;
    }
    if ((b & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
            | ((uint32_t)(s[2] & 0x3F) << 6) | (uint8_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = b;
    return 1;
}

// Display width of a code point in character cells (must match print.c).
static int cp_width(uint32_t cp)
{
    return (cp >= 0x2E80 || cp == 0x2103) ? 2 : 1;  /* 0x2103 = '℃' */
}

static void marquee_draw(void)
{
    char buf[SCREEN_WIDTH * 3 + 1];
    const char *p = marquee_text;
    int col = 0;
    int n = 0;

    /* Advance to the first character that starts at or after the scroll
     * offset, so a CJK glyph is never cut in half at the left edge. */
    while (*p != '\0') {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        if (col >= marquee_offset) {
            break;
        }
        col += cp_width(cp);
        p += len;
    }

    /* Copy characters until the 80-column window is full. */
    col = 0;
    while (*p != '\0' && n < (int)sizeof(buf) - 4) {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        if (col + cp_width(cp) > SCREEN_WIDTH) {
            break;
        }
        for (int i = 0; i < len; i++) {
            buf[n++] = p[i];
        }
        col += cp_width(cp);
        p += len;
    }
    buf[n] = '\0';

    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);
    clear_screen_region(ROW_MARQUEE, 0, ROW_MARQUEE, SCREEN_WIDTH - 1);
    prints(ROW_MARQUEE, 0, buf);
}

static void marquee_scroll(void)
{
    int total = 0;

    for (const char *p = marquee_text; *p != '\0'; ) {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        total += cp_width(cp);
        p += len;
    }

    if (++marquee_offset >= total) {
        marquee_offset = 0;
    }
    marquee_draw();
}

// Advance the marquee if MARQUEE_PERIOD has elapsed since the last step.
// Called from every long-running loop so the banner never freezes.
static void marquee_tick(uint64_t now)
{
    static uint64_t last_tick = 0;

    if (scroll_lock) {
        return;
    }
    if (last_tick == 0 || (now - last_tick) > ((uint64_t)MARQUEE_PERIOD * clks_per_msec)) {
        last_tick = now;
        marquee_scroll();
    }
}

//------------------------------------------------------------------------------
// F1 pop-up: licensing / product information window
//------------------------------------------------------------------------------

void about_popup(void)
{
    save_screen_region(ABOUT_REGION, about_save_buffer);
    set_foreground_colour(WHITE);
    set_background_colour(BLACK);  /* black background, like the config menu */
    clear_screen_region(ABOUT_REGION);

    bool exit_popup = false;
    while (!exit_popup) {
        set_foreground_colour(WHITE);
        set_background_colour(BLACK);
        prints(ABOUT_R+1, ABOUT_C+3, "关于 HeroTest");
        prints(ABOUT_R+3, ABOUT_C+3, "HeroTest是专业的内存颗粒及模组测试");
        prints(ABOUT_R+4, ABOUT_C+3, "系统的一部分。配合专用测试板可以");
        prints(ABOUT_R+5, ABOUT_C+3, "完整的实现颗粒筛选，SLT模组测试，");
        prints(ABOUT_R+6, ABOUT_C+3, "老化测试。此版本授权给 ");
        set_foreground_colour(RED);
        prints(ABOUT_R+6, ABOUT_C+26, "创芯界");
        set_foreground_colour(WHITE);
        prints(ABOUT_R+6, ABOUT_C+32, " 品牌");
        prints(ABOUT_R+7, ABOUT_C+3, "在验货时使用。如需生产线使用的全功");
        prints(ABOUT_R+8, ABOUT_C+3, "能版本，请另行购买。");
        prints(ABOUT_R+11, ABOUT_C+3, "按任意键关闭此窗口");

        if (enable_tty) {
            tty_send_region(ABOUT_REGION);
        }

        // Keep the marquee scrolling while the popup is open.
        if (clks_per_msec > 0) {
            marquee_tick(get_tsc());
        }

        char input_key = get_key();
        if (input_key != '\0') {
            exit_popup = true;
        }
    }

    restore_screen_region(ABOUT_REGION, about_save_buffer);
    set_foreground_colour(palette.foreground);
    set_background_colour(palette.background);
    if (enable_tty) {
        tty_send_region(ABOUT_REGION);
    }
}

void check_input(void)
{
    // printf(18,5, "%u", test_num);

    char input_key = get_key();

    if (input_key == '\0') {
        return;
    } else if (big_status_displayed) {
        restore_big_status();
        enable_big_status = false;
        return;
    }

    switch (input_key) {
      case ESC:
        clear_message_area();
        display_notice("正在重启...");
        reboot();
        break;
      case '1':
        about_popup();
        break;
      case '3':
        set_test_mode(3);
        bail = true;      // abort the current test immediately
        restart = true;
        break;
      case '4':
        set_test_mode(4);
        bail = true;
        restart = true;
        break;
      case '5':
        set_test_mode(5);
        bail = true;
        restart = true;
        break;
      case ' ':
        set_scroll_lock(!scroll_lock);
        break;
      case '\n':
        scroll_wait = false;
        break;
      default:
        break;
    }
}

void set_scroll_lock(bool enabled)
{
    scroll_lock = enabled;
    set_foreground_colour(palette.footer_foreground);
    // The scroll lock hint text is no longer part of the footer, so only a
    // single '*' marker in the last column indicates the locked state.
    printc(ROW_FOOTER, SCREEN_WIDTH - 1, scroll_lock ? '*' : ' ');
    set_foreground_colour(palette.foreground);
}

void toggle_scroll_lock(void)
{
    set_scroll_lock(!scroll_lock);
}

void scroll(void)
{
    if (scroll_message_row < ROW_SCROLL_B) {
        scroll_message_row++;
    } else {
        // Only the master CPU may poll the keyboard, so the scroll-lock
        // single-step wait is only available to it.
        if (smp_my_cpu_num() == master_cpu) {
            if (scroll_lock) {
                display_footer_message("<Enter> 单步执行     ");
            }
            scroll_wait = true;
            do {
                check_input();
            } while (scroll_wait && scroll_lock);

            scroll_wait = false;
            clear_footer_message();
        }
        scroll_screen_region(ROW_SCROLL_T, 0, ROW_SCROLL_B, SCREEN_WIDTH - 1);
    }
}
extern int testpass;

void do_tick(int my_cpu)
{
    static uint64_t last_spinner_tick = 0;
    int act_sec = 0;
    bool use_spin_wait = (power_save < POWER_SAVE_HIGH);
    if (use_spin_wait) {
        barrier_spin_wait(run_barrier);
    } else {
        barrier_halt_wait(run_barrier);
    }

    if (master_cpu == my_cpu) {
        check_input();
        error_update();
    }
    if (use_spin_wait) {
        barrier_spin_wait(run_barrier);
    } else {
        barrier_halt_wait(run_barrier);
    }

    // Only the master CPU does the update.
    if (master_cpu != my_cpu) {
        return;
    }

    test_ticks++;
    pass_ticks++;

    pass_type_t pass_type = (pass_num == 0) ? FAST_PASS : FULL_PASS;

    int pct = 0;
    if (ticks_per_test[pass_type][test_num] > 0) {
        pct = 100 * test_ticks / ticks_per_test[pass_type][test_num];
        if (pct > 100) {
            pct = 100;
        }
    }
    bool update_test = true;
    if (clks_per_msec > 0) {
        uint64_t test_current_time = get_tsc();

        if (test_current_time >= test_next_spin_time) {
            test_next_spin_time = test_current_time + SPINNER_PERIOD*3 * clks_per_msec;
        } else {
            update_test = false;
        }
    }
    // update spinner every SPINNER_PERIOD ms
    if (update_test) {
        display_test_percentage(pct);
        set_foreground_colour(test_bar_colour);
        display_test_bar((BAR_LENGTH * pct) / 100);
        set_foreground_colour(palette.foreground);
    }

    pct = 0;
    if (ticks_per_pass[pass_type] > 0) {
        pct = 100 * pass_ticks / ticks_per_pass[pass_type];
        if (pct > 100) {
            pct = 100;
        }
    }
    display_pass_percentage(pct);
    set_foreground_colour(2); 
    display_pass_bar((BAR_LENGTH * pct) / 100);
    set_foreground_colour(palette.foreground);
    bool update_spinner = true;
    if (clks_per_msec > 0) {
        uint64_t current_time = get_tsc();

        int secs  = (current_time - run_start_time) / (1000 * (uint64_t)clks_per_msec);
        int mins  = secs / 60; secs %= 60; act_sec = secs;
        int hours = mins / 60; mins %= 60;
        display_run_time(hours, mins, secs);

        // Scroll the ad banner on the bottom row. Paused while scroll lock
        // is active (that mode reuses the bottom row for its own message).
        marquee_tick(current_time);

        if (current_time > last_spinner_tick && (current_time - last_spinner_tick) > (200ULL * clks_per_msec)) {
            last_spinner_tick = current_time;
            update_spinner = true;
        } else {
            update_spinner = false;
        }
        // New Dongle polling and checking logic:
        // Run test for 10 seconds, then hang to check dongle.
        static uint64_t last_test_resume_tick = 0;
        static bool first_boot_check = true;
        if (last_test_resume_tick == 0) {
            last_test_resume_tick = current_time;
        }

        if (first_boot_check || (current_time - last_test_resume_tick) > (10000ULL * clks_per_msec)) {
            first_boot_check = false;
            // 10 seconds elapsed, enter hang state to check dongle
            dongle_reset_auth(); // Reset authorization state
            
            uint64_t check_start_tick = get_tsc();
            uint64_t last_send_tick = 0;
            bool need_dongle_msg_shown = false;

            while (1) {
                serial_poll_rx();
                uint64_t now = get_tsc();

                // Keep the marquee scrolling while waiting for the dongle.
                marquee_tick(now);

                // Send heartbeat every 200ms
                if (last_send_tick == 0 || (now - last_send_tick) > (200ULL * clks_per_msec)) {
                    dongle_send_time_packet();
                    last_send_tick = now;
                }

                if (dongle_is_authorized()) {
                    if (need_dongle_msg_shown) {
                        prints(16, 28, "                       ");
                    }
                    break; // Authorized, exit hang loop
                }

                // If 5 seconds pass without authorization, show message
                if (!need_dongle_msg_shown && (now - check_start_tick) > (5000ULL * clks_per_msec)) {
                    set_foreground_colour(RED);
                    prints(16, 28, "需要加密狗！等待中...");
                    set_foreground_colour(palette.foreground);
                    need_dongle_msg_shown = true;
                }
            }
            
            // Update resume tick to start the next 10s normal test phase
            last_test_resume_tick = get_tsc();
        }
    }

    /* ---------------
     * Timed functions
     * --------------- */

    // update spinner every SPINNER_PERIOD ms
    if (update_spinner) {
        spin_idx = (spin_idx + 1) % NUM_SPIN_STATES;
        display_spinner(spin_state[spin_idx]);
    }

    // This only tick one time per second
    if (!timed_update_done) {

        // A corrupted stack canary means a CPU overran its stack slot and may
        // have corrupted the thread-local barrier flags below it (see boot.h).
        static int last_overflow_cpu = -1;
        int overflow_cpu = stack_canary_check();
        if (overflow_cpu >= 0 && overflow_cpu != last_overflow_cpu) {
            last_overflow_cpu = overflow_cpu;
            do_trace(overflow_cpu, "检测到CPU栈溢出 - 测试结果不可靠");
        }

        // Display FAIL banner if (new) errors detected
        if (err_banner_redraw && !big_status_displayed && error_count > 1) {
            display_big_status(false);
        }

        // Check ECC Errors
        memctrl_poll_ecc();

        // Update temperature
        display_temperature();

        // Update TTY one time every TTY_UPDATE_PERIOD second(s)
        if (enable_tty) {

            if (act_sec % tty_update_period == 0) {
                tty_partial_redraw();
            }
            if(testpass == 1)
            {
                direct_send_string("HEROSYS_PASS");
            }
            else if(testpass == 2)
            {
                direct_send_string("HEROSYS_FAIL");
            }
        }

        timed_update_done = true;
    }

    if (act_sec != prev_sec) {
        prev_sec = act_sec;
        timed_update_done = false;
    }
}

void do_trace(int my_cpu, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    spin_lock(error_mutex);
    scroll();
    printi(scroll_message_row, 0, my_cpu, 2, false, false);
    vprintf(scroll_message_row, 4, fmt, args);
    spin_unlock(error_mutex);
    va_end(args);
}

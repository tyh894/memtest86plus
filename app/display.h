// SPDX-License-Identifier: GPL-2.0
#ifndef DISPLAY_H
#define DISPLAY_H
/**
 * \file
 *
 * Provides (macro) functions that implement the UI display.
 * All knowledge about the display layout is encapsulated here.
 *
 *//*
 * Copyright (C) 2020-2022 Martin Whitaker.
 * Copyright (C) 2004-2025 Sam Demeulemeester.
 */

#include <stdbool.h>

#include "screen.h"

#include "print.h"
#include "string.h"

#include "test.h"

#define ROW_SPD         10

#define ROW_MESSAGE_T   10
#define ROW_MESSAGE_B   (SCREEN_HEIGHT - 4)

#define ROW_SCROLL_T    (ROW_MESSAGE_T + 2)
#define ROW_SCROLL_B    (SCREEN_HEIGHT - 4)

#define ROW_FOOTER      (SCREEN_HEIGHT - 2)

#define BAR_LENGTH      40

#define ERROR_LIMIT     UINT64_C(999999999999)

#define TEMP_LEN(t) ((t) < 0 ? ((t) <= -10 ? 3 : 2) : ((t) >= 100 ? 3 : (t) >= 10 ? 2 : 1))

typedef enum {
    DISPLAY_MODE_NA,
    DISPLAY_MODE_SPD,
    DISPLAY_MODE_IMC
} display_mode_t;

// CPU model (e.g. "AMD ...") removed from the title row. Kept as a no-op
// so call sites stay unchanged (reports still log the CPU model).
#define display_cpu_model(str)

#define display_cpu_clk(freq) \
    printf(1, 11, "%iMHz", freq)

#define display_cpu_temperature(actual_cpu_temp, max_cpu_temp, offset) \
    { \
        clear_screen_region(1, 18, 1, 27); \
        printf(1, 25 - offset, "%i/%i℃", actual_cpu_temp, max_cpu_temp); \
    }

#define display_cpu_addr_mode(str) \
    prints(4, 75, str)

#define display_l1_cache_size(size) \
    printf(2, 10, "%6kB", (uintptr_t)(size))

#define display_l2_cache_size(size) \
    printf(3, 10, "%6kB", (uintptr_t)(size))

#define display_l3_cache_size(size) \
    printf(4, 10, "%6kB", (uintptr_t)(size))

#define display_memory_size(size) \
    printf(5, 10, "%6kB", (uintptr_t)(size))

#define display_l1_cache_speed(speed) \
    printf(2, 18, "%S6kB/s", (uintptr_t)(speed))

#define display_l2_cache_speed(speed) \
    printf(3, 18, "%S6kB/s", (uintptr_t)(speed))

#define display_l3_cache_speed(speed) \
    printf(4, 18, "%S6kB/s", (uintptr_t)(speed))

#define display_ram_speed(speed) \
    printf(5, 18, "%S6kB/s", (uintptr_t)(speed))

#define display_ram_temperature(ram_temp, idx) \
    { \
        clear_screen_region(idx+ROW_SPD, SCREEN_WIDTH-5, idx+ROW_SPD, SCREEN_WIDTH-1); \
        printf(idx+ROW_SPD, SCREEN_WIDTH-4, "%i℃", ram_temp); \
    }

#define display_status(status) \
    prints(7, 66, status)

// CPU / SMP / memory spec info removed from the status line (left side is
// now occupied by Pass/Errors). Kept as no-ops so call sites stay unchanged.
#define display_threading(nb, mode)

#define display_threading_disabled()

#define display_cpu_topo_hybrid(num_pcores, num_ecores, num_threads)

#define display_cpu_topo_hybrid_short(num_threads)

#define display_cpu_topo_multi_socket(num_sockets, num_cores, num_threads)

#define display_cpu_topo( num_cores, num_threads)

#define display_cpu_topo_short( num_cores, num_threads)

#define display_spec_mode(mode)

#define display_spec_ddr5(freq, type, cl, cl_dec, rcd, rp, ras)

#define display_spec_ddr(freq, type, cl, cl_dec, rcd, rp, ras)

#define display_spec_sdr(freq, type, cl, rcd, rp, ras)

#define display_dmi_mb(sys_ma, sys_sku) \
    // dmicol = prints(ROW_FOOTER - 1, dmicol, sys_man); \
    // prints(ROW_FOOTER - 1, dmicol + 1, sys_sku);

#define display_active_cpu(cpu_num)

#define display_all_active()

#define display_spinner(spin_state) \
    printc(7, 77, spin_state)

#define display_pass_percentage(pct) \
    printi(1, 34, pct, 3, false, false)

#define display_pass_bar(length) \
    while (length > pass_bar_length) {          \
        printc(1, 39 + pass_bar_length, '#');   \
        pass_bar_length++;                      \
    }

#define display_test_percentage(pct) \
    printi(2, 34, pct, 3, false, false)

#define display_test_bar(length) \
    while (length > test_bar_length) {          \
        printc(2, 39 + test_bar_length, '#');   \
        test_bar_length++;                      \
    }

#define display_test_number(number) \
    printi(3, 36, number, 2, false, true)

#define display_test_description(str) \
    prints(3, 39, str)

#define display_test_addresses(pb, pe, total) \
    { \
        clear_screen_region(4, 39, 4, SCREEN_WIDTH - 6); \
        printf(4, 39, "%kB - %kB [%kB / %kB]", pb, pe, (pe) - (pb), total); \
    }

#define display_test_stage_description(...) \
    { \
        clear_screen_region(4, 39, 4, SCREEN_WIDTH - 6); \
        printf(4, 39, __VA_ARGS__); \
    }

#define display_test_pattern_name(str) \
    { \
        clear_screen_region(5, 39, 5, SCREEN_WIDTH - 1); \
        prints(5, 39, str); \
    }

#define display_test_pattern_value(pattern) \
    { \
        clear_screen_region(5, 39, 5, SCREEN_WIDTH - 1); \
        printf(5, 39, "0x%0*x", TESTWORD_DIGITS, pattern); \
    }

#define display_test_pattern_values(pattern, offset) \
    { \
        clear_screen_region(5, 39, 5, SCREEN_WIDTH - 1); \
        printf(5, 39, "0x%0*x - %i", TESTWORD_DIGITS, pattern, offset); \
    }

#define display_run_time(hours, mins, secs) \
    printf(7, 50, "%2i:%02i:%02i", hours, mins, secs)

#define display_pass_count(count) \
    printi(7, 7, count, 0, false, true)

#define display_err_count_without_ecc(count) \
    printi(7, 22, count, 0, false, true)

#define display_err_count_with_ecc(count_err, count_ecc) \
    { \
        printi(7, 22, count_err, 0, false, true); \
        printi(7, 33, count_ecc, 0, false, true); \
    }

#define clear_message_area() \
    { \
        clear_screen_region(ROW_MESSAGE_T, 0, ROW_MESSAGE_B, SCREEN_WIDTH - 1); \
        scroll_message_row = ROW_SCROLL_T - 1; \
    }

#define display_pinned_message(row, col, ...) \
    printf(ROW_MESSAGE_T + row, col, __VA_ARGS__)

#define display_scrolled_message(col, ...) \
    printf(scroll_message_row, col, __VA_ARGS__)

#define display_notice(str) \
    prints(ROW_MESSAGE_T + 8, (SCREEN_WIDTH - str_width(str)) / 2, str)

#define display_notice_with_args(length, ...) \
    printf(ROW_MESSAGE_T + 8, (SCREEN_WIDTH - length) / 2, __VA_ARGS__)

#define clear_footer_message() \
    { \
        set_background_colour(palette.foreground); \
        clear_screen_region(ROW_FOOTER + 1, 0, ROW_FOOTER + 1, SCREEN_WIDTH - 1); \
        set_background_colour(palette.background);  \
    }

#define display_footer_message(str) \
    { \
        set_foreground_colour(palette.footer_foreground);  \
        prints(ROW_FOOTER + 1, 0, str);  \
        set_foreground_colour(palette.footer_background); \
    }

#define trace(my_cpu, ...) \
    if (enable_trace) do_trace(my_cpu, __VA_ARGS__)

#define display_msr_failed_flag() \
    printc(0, SCREEN_WIDTH - 1, '*');

extern int scroll_message_row;

extern uint64_t run_start_time;

extern display_mode_t display_mode;

extern screen_palette_t palette;

void display_init(void);

void display_cpu_topology(void);

void post_display_init(void);

void display_start_run(void);

void display_start_pass(void);

void display_start_test(void);

void display_error_count(void);

void display_temperature(void);

void display_big_status(bool pass);

void restore_big_status(void);

void check_input(void);

void set_scroll_lock(bool enabled);

void toggle_scroll_lock(void);

void scroll(void);

void do_tick(int my_cpu);

void do_trace(int my_cpu, const char *fmt, ...);

#endif // DISPLAY_H

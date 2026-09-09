// SPDX-License-Identifier: GPL-2.0
#ifndef CONFIG_H
#define CONFIG_H
/**
 * \file
 *
 * Provides the configuration settings and pop-up menu.
 *
 *//*
 * Copyright (C) 2020-2022 Martin Whitaker.
 */

#include <stdbool.h>
#include <stdint.h>

#include "smp.h"
#include "cpuid.h"

typedef enum {
    PAR,
    SEQ,
    ONE
} cpu_mode_t;

typedef enum {
    ERROR_MODE_NONE,
    ERROR_MODE_SUMMARY,
    ERROR_MODE_ADDRESS,
    ERROR_MODE_BADRAM,
    ERROR_MODE_MEMMAP,
    ERROR_MODE_PAGES,
} error_mode_t;

typedef enum {
    POWER_SAVE_OFF,
    POWER_SAVE_LOW,
    POWER_SAVE_HIGH
} power_save_t;

extern uintptr_t    pm_limit_lower;
extern uintptr_t    pm_limit_upper;

extern uintptr_t    num_pages_to_test;

extern cpu_mode_t   cpu_mode;

extern error_mode_t error_mode;

extern cpu_state_t  cpu_state[MAX_CPUS];

extern core_type_t  hybrid_core_type[MAX_CPUS];
extern bool         exclude_ecores;

extern bool         smp_enabled;

extern bool         enable_big_status;
extern bool         enable_trace;

extern bool         enable_temp_cpu;
extern bool         enable_temp_ram;

extern bool         enable_sm;
extern bool         enable_spd_crc;
extern bool         enable_tty;
extern bool         enable_tty_log;
extern int          log_max_passes;
extern bool         enable_bench;
extern bool         enable_mch_read;
extern bool         enable_ecc_polling;
extern bool         enable_numa;

extern bool         pause_at_start;
extern bool         dark_mode;

extern power_save_t power_save;

extern uintptr_t    tty_address;
extern int          tty_baud_rate;
extern int          tty_update_period;
extern bool         tty_new_line;

extern uint32_t     tty_mmio_ref_clk;
extern int          tty_mmio_stride;
extern bool         tty_pl011;

extern bool err_banner_redraw;

// Configuration struct for saving to USB
#define APP_CONFIG_MAGIC   0x4D543836 // 'MT86'
#define APP_CONFIG_VERSION 1

// We hardcode the max number to avoid circular dependency issues 
// between tests.h and config.h when included from system files
#define CONFIG_MAX_TESTS   12

typedef struct {
    uint32_t magic;
    uint32_t version;
    int max_pass_num;
    bool continue_on_error;
    bool test_enabled[CONFIG_MAX_TESTS];
    int test_sequence[CONFIG_MAX_TESTS];
    // Add future configuration items here
} app_config_t;

void config_init(void);

void config_menu(bool initial);

void initial_config(void);

void save_app_config(void);

#endif // CONFIG_H

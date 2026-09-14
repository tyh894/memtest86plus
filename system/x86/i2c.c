// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2004-2024 Sam Demeulemeester

#include "display.h"

#include "io.h"
#include "tsc.h"
#include "pci.h"
#include "unistd.h"
#include "string.h"
#include "macros.h"

#include "cpuid.h"
#include "cpuinfo.h"
#include "hwquirks.h"
#include "i2c_x86.h"
#include "memctrl.h"
#include "smbios.h"
#include "spd.h"

int smbbus, smbdev, smbfun;
unsigned short smbusbase = 0;
uint32_t smbus_id = 0;
static uint16_t extra_initial_sleep_for_smb_transaction = 0;

static int8_t spd_page = -1;
static int8_t last_adr = -1;

static uint8_t last_smb_status = 0;
static uint8_t last_smb_rc = 0;
static uint8_t last_smb_addr = 0;
static uint16_t last_smb_cmd = 0;

typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t fun;
    uint16_t vid;
    uint16_t did;
} smbus_candidate_t;

static uint8_t smbus_class_count = 0;
static uint8_t smbus_candidates_count = 0;
static smbus_candidate_t smbus_candidates[4];
// Functions Prototypes
static bool setup_smb_controller(void);
static bool find_smb_controller(uint16_t vid, uint16_t did);
static bool is_smbus_pci_class(void);

static bool nv_mcp_get_smb(void);
static bool amd_sb_get_smb(void);
static bool fch_zen_get_smb(void);
static bool piix4_get_smb(uint8_t address);
static bool ich5_get_smb(void);
static bool ali_get_smb(uint8_t address);
static uint8_t ich5_process(void);
static uint8_t ich5_read_spd_byte(uint8_t adr, uint16_t cmd);
static uint8_t nf_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr);
static uint8_t ali_m1563_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr);
static uint8_t amd_spd_read_byte(uint8_t slot_idx, uint16_t spd_adr);
static uint8_t ali_m1543_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr);

static void enable_smbus_io_decode(void)
{
    uint16_t cmd = pci_config_read16(smbbus, smbdev, smbfun, 0x04);
    if ((cmd & 0x01) == 0) {
        pci_config_write16(smbbus, smbdev, smbfun, 0x04, cmd | 0x01);
    }
}

static void reset_smbus_host_controller(void)
{
    __outb(__inb(SMBHSTSTS) & 0x1F, SMBHSTSTS);
    usleep(1000);
}

static void force_smbus_release(void)
{
    uint8_t ctl = __inb(SMBHSTCNT);
    __outb(ctl | SMBHSTCNT_KILL, SMBHSTCNT);
    usleep(2000);

    uint8_t status = __inb(SMBHSTSTS);
    __outb(status & 0x1F, SMBHSTSTS);
    __outb(SMBHSTSTS_INUSE_STS, SMBHSTSTS);
    usleep(1000);
}
int print_spd_startup_info(void)
{
    uint8_t spdidx = 0, spd_line_idx = 0;

    spd_info curspd;

    // The RAM temperature display is driven from this table - rebuild it
    // from scratch on every enumeration.
    memset(ram_slot_info, 0, sizeof(ram_slot_info));

    if (quirk.type & QUIRK_TYPE_SMBUS) {
        quirk.process();
    }
    bool has_smbus = setup_smb_controller() && smbusbase != 0;
     if (!has_smbus && dmi_memory_device_count == 0) {
        prints(ROW_SPD-2, 0, "Memory SPD Information");
        prints(ROW_SPD-1, 0, "----------------------");
        printf(ROW_SPD, 0, "SMBus not detected (id=0x%x base=0x%x)", smbus_id, smbusbase);
        return 0;
    }
    
    spd_info physical_spds[MAX_SPD_SLOT];
    int physical_count = 0;

    for (spdidx = 0; spdidx < MAX_SPD_SLOT; spdidx++) {
        spd_info temp_spd;
        memset(&temp_spd, 0, sizeof(temp_spd));
        parse_spd(&temp_spd, spdidx);
        if (temp_spd.isValid) {
            temp_spd.slot_num = spdidx;
            physical_spds[physical_count++] = temp_spd;
        }
    }

    // If we only printed from SMBIOS fallback, show (SMBIOS) in title
    // Also, if physical_count is 0 but we have dmi_memory_device_count, we use SMBIOS fallback
    // Or if we found physical spd but matched none of them with DMI
    bool used_smbios = false;
    if (physical_count == 0 && dmi_memory_device_count > 0) {
        used_smbios = true;
    } else if (physical_count > 0 && dmi_memory_device_count > 0) {
        // Assume physical unless proven otherwise (check if we matched any)
        // We will compute matched physical later, but basically if we have physical_count > 0 we can just say physical.
        used_smbios = false;
    }

    // If no SPD devices were found natively, fallback completely to SMBIOS DMI
    if (used_smbios) {
        if (has_smbus) {
            prints(ROW_SPD-1, 0, "Memory SPD Information (SMBIOS)");
        } else {
            prints(ROW_SPD-1, 0, "Memory SPD Information (SMBIOS, No SMBus)");
        }
    } else {
        prints(ROW_SPD-1, 0, "Memory SPD Information");
    }
    // We omit the "----" separator to save one line for 12-slot systems.

    int pop_dmi[MAX_SPD_SLOT];
    int pop_dmi_count = 0;
    for (int i = 0; i < dmi_memory_device_count; i++) {
        struct mem_dev *md = dmi_memory_devices[i];
        if (md && md->size != 0 && md->size != 0xFFFF) {
            if (md->size == 0x7FFF && md->header.length >= 0x20) {
                uint32_t ext_size = *(uint32_t *)((uint8_t *)md + 0x1C);
                if (ext_size == 0) continue;
            } else if (md->size == 0x7FFF) {
                continue;
            }
            if (pop_dmi_count < MAX_SPD_SLOT) {
                pop_dmi[pop_dmi_count++] = i;
            }
        }
    }

    // If DMI data is empty, fallback to pure physical display
    if (dmi_memory_device_count == 0) {
        int display_slots = 0;
        if (physical_count > 0) {
            int max_slot = 0;
            for (int j = 0; j < physical_count; j++) {
                if (physical_spds[j].slot_num > max_slot) {
                    max_slot = physical_spds[j].slot_num;
                }
            }
            display_slots = max_slot + 1;
            if (display_slots <= 2) display_slots = 2;
            else if (display_slots <= 4) display_slots = 4;
            else display_slots = 8;
        }

        for (int i = 0; i < display_slots && spd_line_idx < MAX_SPD_SLOT; i++) {
            bool found = false;
            for (int j = 0; j < physical_count; j++) {
                if (physical_spds[j].slot_num == i) {
                    if (strstr(physical_spds[j].sku, "0x105E") != NULL || strstr(physical_spds[j].sku, "0x105e") != NULL) {
                        char temp_sku[SPD_SKU_LEN];
                        for (int k = 0; k < SPD_SKU_LEN; k++) {
                            temp_sku[k] = physical_spds[j].sku[k];
                        }
                        
                        char *sku_ptr = temp_sku;
                        if (strncmp(temp_sku, "Unknown (0x105E) ", 17) == 0) {
                            sku_ptr = temp_sku + 17;
                        } else if (strncmp(temp_sku, "Unknown (0x105e) ", 17) == 0) {
                            sku_ptr = temp_sku + 17;
                        }
                        
                        const char *hero_str = "HEROSYS ";
                        int dest_idx = 0;
                        while (hero_str[dest_idx] && dest_idx < SPD_SKU_LEN - 1) {
                            physical_spds[j].sku[dest_idx] = hero_str[dest_idx];
                            dest_idx++;
                        }
                        while (*sku_ptr && dest_idx < SPD_SKU_LEN - 1) {
                            physical_spds[j].sku[dest_idx++] = *sku_ptr++;
                        }
                        physical_spds[j].sku[dest_idx] = '\0';
                        
                        physical_spds[j].jedec_code = 0xFFFF;
                    }
                    print_spdi(physical_spds[j], ROW_SPD+spd_line_idx);
                    ram_slot_info[physical_spds[j].slot_num].slot_idx = physical_spds[j].slot_num;
                    ram_slot_info[physical_spds[j].slot_num].display_idx = spd_line_idx;
                    ram_slot_info[physical_spds[j].slot_num].isPopulated = true;
                    ram_slot_info[physical_spds[j].slot_num].hasTempSensor = physical_spds[j].hasTempSensor;
                    found = true;
                    break;
                }
            }
            if (!found) {
                spd_info empty_spd;
                memset(&empty_spd, 0, sizeof(empty_spd));
                empty_spd.slot_num = i;
                empty_spd.module_size = 0;
                print_spdi(empty_spd, ROW_SPD+spd_line_idx);
            }
            spd_line_idx++;
        }
        return;
    }

    // ========== Fallback pure DMI logic ==========
    int display_indexes[MAX_SPD_SLOT];
    int display_count = 0;

    if (dmi_memory_device_count <= MAX_SPD_SLOT) {
        for (int i = 0; i < dmi_memory_device_count; i++) {
            display_indexes[display_count++] = i;
        }
    } else {
        // More than MAX_SPD_SLOT slots. Prioritize populated ones.
        for (int i = 0; i < dmi_memory_device_count; i++) {
            struct mem_dev *md = dmi_memory_devices[i];
            if (md && md->size != 0 && md->size != 0xFFFF && md->size != 0x7FFF) {
                if (display_count < MAX_SPD_SLOT) {
                    display_indexes[display_count++] = i;
                }
            } else if (md && md->size == 0x7FFF && md->header.length >= 0x20) {
                // Ext size check
                uint32_t ext_size = *(uint32_t *)((uint8_t *)md + 0x1C);
                if (ext_size != 0) {
                    if (display_count < MAX_SPD_SLOT) {
                        display_indexes[display_count++] = i;
                    }
                }
            }
        }
        // Fill the rest with empty ones, but only up to MAX_SPD_SLOT
        for (int i = 0; i < dmi_memory_device_count && display_count < MAX_SPD_SLOT; i++) {
            bool already_added = false;
            for (int j = 0; j < display_count; j++) {
                if (display_indexes[j] == i) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added) {
                display_indexes[display_count++] = i;
            }
        }
        
        // Sort display_indexes to keep them in order
        for (int i = 0; i < display_count - 1; i++) {
            for (int j = i + 1; j < display_count; j++) {
                if (display_indexes[i] > display_indexes[j]) {
                    int tmp = display_indexes[i];
                    display_indexes[i] = display_indexes[j];
                    display_indexes[j] = tmp;
                }
            }
        }
    }

    spd_info dmi_spds_array[MAX_SPD_SLOT];
    bool dmi_spd_valid[MAX_SPD_SLOT];
    for(int k=0; k<MAX_SPD_SLOT; k++) dmi_spd_valid[k] = false;

    for (int k = 0; k < display_count && k < MAX_SPD_SLOT; k++) {
        int i = display_indexes[k];
        struct mem_dev *md = dmi_memory_devices[i];
        if (md == NULL) continue;

        memset(&curspd, 0, sizeof(curspd));
        curspd.isValid = true; // Assume valid slot
        curspd.slot_num = i;

        // Get DMI location strings
        char *dev_loc = smbios_get_string(&md->header, md->dev_locator);
        char *bank_loc = smbios_get_string(&md->header, md->bank_locator);

        if (dev_loc && (strncmp(dev_loc, "NO DIMM", 7) == 0 || strncmp(dev_loc, "Unknown", 7) == 0)) {
            dev_loc = NULL;
        }
        if (bank_loc && (strncmp(bank_loc, "NO DIMM", 7) == 0 || strncmp(bank_loc, "Unknown", 7) == 0)) {
            bank_loc = NULL;
        }
        
        if (dev_loc) {
            if (bank_loc && strncmp(bank_loc, "BANK ", 5) == 0) {
                bank_loc = NULL;
            }

            if (strncmp(dev_loc, "Controller", 10) == 0 && strstr(dev_loc, "-Channel")) {
                char *channel_ptr = strstr(dev_loc, "-Channel");
                char ch = *(channel_ptr + 8); // 'A', 'B', etc.
                char *dimm_ptr = strstr(dev_loc, "-DIMM");
                int n = 0;
                if (dimm_ptr) {
                    curspd.slot_name[n++] = 'C';
                    curspd.slot_name[n++] = 'h';
                    curspd.slot_name[n++] = ch;
                    while (*dimm_ptr && n < (int)sizeof(curspd.slot_name) - 1) {
                        curspd.slot_name[n++] = *dimm_ptr++;
                    }
                    curspd.slot_name[n] = '\0';
                } else {
                    while (*dev_loc && n < (int)sizeof(curspd.slot_name) - 1) {
                        curspd.slot_name[n++] = *dev_loc++;
                    }
                    curspd.slot_name[n] = '\0';
                }
            } else if (bank_loc) {
                int n = 0;
                while (*bank_loc && n < (int)sizeof(curspd.slot_name) - 2) {
                    curspd.slot_name[n++] = *bank_loc++;
                }
                curspd.slot_name[n++] = ' ';
                while (*dev_loc && n < (int)sizeof(curspd.slot_name) - 1) {
                    curspd.slot_name[n++] = *dev_loc++;
                }
                curspd.slot_name[n] = '\0';
            } else {
                int n = 0;
                while (*dev_loc && n < (int)sizeof(curspd.slot_name) - 1) {
                    curspd.slot_name[n++] = *dev_loc++;
                }
                curspd.slot_name[n] = '\0';
            }
        }

        bool matched_physical = false;

        // Try to match physical SPD data with DMI slot sequence
        if (md->size > 0 && pop_dmi_count > 0) {
            int relative_rank = -1;
            for(int rank = 0; rank < pop_dmi_count; rank++) {
                if (pop_dmi[rank] == i) {
                    relative_rank = rank;
                    break;
                }
            }
            
            // Found matching populated slot
            if (relative_rank != -1 && relative_rank < physical_count) {
                // Keep the DMI slot name but use physical SPD data
                char temp_name[16];
                memcpy(temp_name, curspd.slot_name, sizeof(temp_name));
                
                curspd = physical_spds[relative_rank];
                memcpy(curspd.slot_name, temp_name, sizeof(temp_name));
                
                matched_physical = true;
            }
        }

        // If physical SPD matching failed, fallback to SMBus SMBIOS data
        if (!matched_physical) {
            if (md->size == 0 || md->size == 0xFFFF) {
                // Empty slot
                curspd.module_size = 0;
            } else {
                // Fallback: extract capacity from DMI
                if (md->size != 0x7FFF) {
                    if (md->size & 0x8000) {
                        curspd.module_size = (md->size & 0x7FFF) / 1024;
                    } else {
                        curspd.module_size = md->size;
                    }
                } else if (md->header.length >= 0x20) {
                    uint32_t ext_size = *(uint32_t *)((uint8_t *)md + 0x1C);
                    curspd.module_size = ext_size;
                }
                switch (md->type) {
                    case DMI_DDR: curspd.type = "DDR"; break;
                    case DMI_DDR2: curspd.type = "DDR2"; break;
                    case DMI_DDR3: curspd.type = "DDR3"; break;
                    case DMI_DDR4: curspd.type = "DDR4"; break;
                    case DMI_DDR5: curspd.type = "DDR5"; break;
                    case DMI_LPDDR3: curspd.type = "LPDDR3"; break;
                    case DMI_LPDDR4: curspd.type = "LPDDR4"; break;
                    case DMI_LPDDR5: curspd.type = "LPDDR5"; break;
                    default: curspd.type = "Unk"; break;
                }
                if (md->header.length > offsetof(struct mem_dev, speed)) {
                    curspd.freq = (md->speed == 0xFFFF) ? 0 : md->speed;
                } else {
                    curspd.freq = 0;
                }

                // No physical SPD match: guess the I2C slot from the populated
                // DMI slot order (rank 0 -> SPD address 0x50, etc).
                for (int r = 0; r < pop_dmi_count; r++) {
                    if (pop_dmi[r] == i) {
                        curspd.slot_num = (uint8_t)r;
                        break;
                    }
                }
                // All DDR5 modules have an SPD5118 hub with a temperature sensor.
                if (md->type == DMI_DDR5) {
                    curspd.hasTempSensor = true;
                }
            }

            // ALWAYS extract strings, even if size is 0!
            char *manuf = NULL;
            char *partnum = NULL;
            if (md->header.length > offsetof(struct mem_dev, manufacturer)) {
                manuf = smbios_get_string(&md->header, md->manufacturer);
            }
            if (md->header.length > offsetof(struct mem_dev, partnum)) {
                partnum = smbios_get_string(&md->header, md->partnum);
            }

            if (manuf && (strncmp(manuf, "Undefined", 9) == 0 || strncmp(manuf, "Unknown", 7) == 0 || strncmp(manuf, "NO DIMM", 7) == 0)) {
                manuf = NULL;
            }
            if (partnum && (strncmp(partnum, "Undefined", 9) == 0 || strncmp(partnum, "Unknown", 7) == 0 || strncmp(partnum, "NO DIMM", 7) == 0)) {
                partnum = NULL;
            }
            
            int sku_idx = 0;
            
            // If manufacturer is Unknown, Undefined, or Noname, ignore it to prevent ugly prefixes
            if (manuf && (strncmp(manuf, "Unknown", 7) == 0 || strncmp(manuf, "Undefined", 9) == 0 || strncmp(manuf, "Noname", 6) == 0)) {
                manuf = NULL;
            }
            
            if (manuf) {
                while (*manuf && sku_idx < SPD_SKU_LEN - 1) {
                    curspd.sku[sku_idx++] = *manuf++;
                }
            }
            if (sku_idx > 0 && partnum && sku_idx < SPD_SKU_LEN - 1) {
                curspd.sku[sku_idx++] = ' ';
            }
            if (partnum) {
                // Check if partnum contains "0x105e" and force HEROSYS prefix if so
                if (strstr(partnum, "0x105e") != NULL || strstr(partnum, "0x105E") != NULL) {
                    const char* hero = "HEROSYS ";
                    while (*hero && sku_idx < SPD_SKU_LEN - 1) {
                        curspd.sku[sku_idx++] = *hero++;
                    }
                }
                while (*partnum && sku_idx < SPD_SKU_LEN - 1) {
                    curspd.sku[sku_idx++] = *partnum++;
                }
            }
            curspd.sku[sku_idx] = '\0';

            // HACK: If module size is 0 but we have a valid part number, it's a hidden populated slot!
            if (curspd.module_size == 0 && sku_idx > 0) {
                curspd.module_size = 16384; // Force it to show
                curspd.type = "DDR4"; // Guess fallback
                curspd.freq = 2667;
            }
        }

        // ??????
        if (curspd.module_size > 0 && (strstr(curspd.sku, "0x105E") != NULL || strstr(curspd.sku, "0x105e") != NULL)) {
            char temp_sku[SPD_SKU_LEN];
            for (int k = 0; k < SPD_SKU_LEN; k++) {
                temp_sku[k] = curspd.sku[k];
            }
            
            char *sku_ptr = temp_sku;
            if (strncmp(temp_sku, "Unknown (0x105E) ", 17) == 0) {
                sku_ptr = temp_sku + 17;
            } else if (strncmp(temp_sku, "Unknown (0x105e) ", 17) == 0) {
                sku_ptr = temp_sku + 17;
            }
            
            const char *hero_str = "HEROSYS ";
            int dest_idx = 0;
            while (hero_str[dest_idx] && dest_idx < SPD_SKU_LEN - 1) {
                curspd.sku[dest_idx] = hero_str[dest_idx];
                dest_idx++;
            }
            while (*sku_ptr && dest_idx < SPD_SKU_LEN - 1) {
                curspd.sku[dest_idx++] = *sku_ptr++;
            }
            curspd.sku[dest_idx] = '\0';
            
            curspd.jedec_code = 0xFFFF;
        }

        dmi_spds_array[k] = curspd;
        dmi_spd_valid[k] = true;
    }

    // Now compute majority SKU
    char majority_sku[SPD_SKU_LEN] = {0};
    int max_count = 0;

    for (int k = 0; k < display_count && k < MAX_SPD_SLOT; k++) {
        if (!dmi_spd_valid[k]) continue;
        if (dmi_spds_array[k].module_size > 0 && dmi_spds_array[k].sku[0] != '\0') {
            int count = 0;
            for (int m = 0; m < display_count && m < MAX_SPD_SLOT; m++) {
                if (dmi_spd_valid[m] && dmi_spds_array[m].module_size > 0) {
                    bool match = true;
                    for (int c = 0; c < SPD_SKU_LEN; c++) {
                        if (dmi_spds_array[k].sku[c] != dmi_spds_array[m].sku[c]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) count++;
                }
            }
            if (count > max_count) {
                max_count = count;
                for (int c = 0; c < SPD_SKU_LEN; c++) majority_sku[c] = dmi_spds_array[k].sku[c];
            }
        }
    }

    // Print all
    for (int k = 0; k < display_count && spd_line_idx < MAX_SPD_SLOT; k++) {
        if (!dmi_spd_valid[k]) continue;
        
        spd_info print_spd = dmi_spds_array[k];
        bool is_diff = false;

        if (print_spd.module_size > 0 && majority_sku[0] != '\0') {
            for (int c = 0; c < SPD_SKU_LEN; c++) {
                if (print_spd.sku[c] != majority_sku[c]) {
                    is_diff = true;
                    break;
                }
            }
        }

        if (is_diff) {
            set_foreground_colour(RED);
        }
        print_spdi(print_spd, ROW_SPD + spd_line_idx);
        if (is_diff) {
            set_foreground_colour(palette.foreground);
        }
        if (print_spd.module_size > 0 && print_spd.slot_num < MAX_SPD_SLOT) {
            ram_slot_info[print_spd.slot_num].slot_idx = print_spd.slot_num;
            ram_slot_info[print_spd.slot_num].display_idx = spd_line_idx;
            ram_slot_info[print_spd.slot_num].isPopulated = true;
            ram_slot_info[print_spd.slot_num].hasTempSensor = print_spd.hasTempSensor;
        }
        spd_line_idx++;
    }

    return spd_line_idx;
}

// --------------------------
// SMBUS Controller Functions
// --------------------------

static bool setup_smb_controller(void)
{
    uint16_t vid, did;

    bool fallback_valid = false;
    uint8_t fallback_bus = 0;
    uint8_t fallback_dev = 0;
    uint8_t fallback_fun = 0;
    uint16_t fallback_vid = 0;
    uint16_t fallback_did = 0;

    smbus_class_count = 0;
    smbus_candidates_count = 0;

    static const uint8_t fast_scan_buses[] = { 0x00, 0x80 };

    for (unsigned int i = 0; i < ARRAY_SIZE(fast_scan_buses); i++) {
        smbbus = fast_scan_buses[i];
        for (smbdev = 0; smbdev < 32; smbdev++) {
            for (smbfun = 0; smbfun < 8; smbfun++) {
                vid = pci_config_read16(smbbus, smbdev, smbfun, 0);
                if (vid != 0xFFFF) {
                    if (is_smbus_pci_class()) {
                        smbus_class_count++;
                        if (smbus_candidates_count < ARRAY_SIZE(smbus_candidates)) {
                            smbus_candidates[smbus_candidates_count++] = (smbus_candidate_t){
                                .bus = (uint8_t)smbbus,
                                .dev = (uint8_t)smbdev,
                                .fun = (uint8_t)smbfun,
                                .vid = vid,
                                .did = pci_config_read16(smbbus, smbdev, smbfun, 2),
                            };
                        }
                    }
                    did = pci_config_read16(smbbus, smbdev, smbfun, 2);
                    if (did != 0xFFFF) {
                        if (find_smb_controller(vid, did)) {
                           bool any_spd = false;
                            for (uint8_t slot = 0; slot < MAX_SPD_SLOT; slot++) {
                                if (get_spd(slot, 0) != 0xFF) {
                                    any_spd = true;
                                    break;
                                }
                            }
                            if (any_spd) {
                                return true;
                            }
                            bool current_is_amd_family = (vid == PCI_VID_AMD || vid == PCI_VID_HYGON || vid == PCI_VID_ATI);
                            bool fallback_is_amd_family = (fallback_vid == PCI_VID_AMD || fallback_vid == PCI_VID_HYGON || fallback_vid == PCI_VID_ATI);
                            if (!fallback_valid || (current_is_amd_family && !fallback_is_amd_family)) {
                                fallback_valid = true;
                                fallback_bus = smbbus;
                                fallback_dev = smbdev;
                                fallback_fun = smbfun;
                                fallback_vid = vid;
                                fallback_did = did;
                            }
                            continue;
                        }
                    }
                }
            }
        }
    }

    for (smbbus = 0; smbbus < 0x100; smbbus++) {
        if (smbbus == 0x00 || smbbus == 0x80) {
            continue;
        }
        for (smbdev = 0; smbdev < 32; smbdev++) {
            for (smbfun = 0; smbfun < 8; smbfun++) {
                vid = pci_config_read16(smbbus, smbdev, smbfun, 0);
                if (vid != 0xFFFF) {
                    if (is_smbus_pci_class()) {
                        smbus_class_count++;
                        if (smbus_candidates_count < ARRAY_SIZE(smbus_candidates)) {
                            smbus_candidates[smbus_candidates_count++] = (smbus_candidate_t){
                                .bus = (uint8_t)smbbus,
                                .dev = (uint8_t)smbdev,
                                .fun = (uint8_t)smbfun,
                                .vid = vid,
                                .did = pci_config_read16(smbbus, smbdev, smbfun, 2),
                            };
                        }
                    }
                    did = pci_config_read16(smbbus, smbdev, smbfun, 2);
                    if (did != 0xFFFF) {
                        if (find_smb_controller(vid, did)) {
                            bool any_spd = false;
                            for (uint8_t slot = 0; slot < MAX_SPD_SLOT; slot++) {
                                if (get_spd(slot, 0) != 0xFF) {
                                    any_spd = true;
                                    break;
                                }
                            }
                            if (any_spd) {
                                return true;
                            }
                            bool current_is_amd_family = (vid == PCI_VID_AMD || vid == PCI_VID_HYGON || vid == PCI_VID_ATI);
                            bool fallback_is_amd_family = (fallback_vid == PCI_VID_AMD || fallback_vid == PCI_VID_HYGON || fallback_vid == PCI_VID_ATI);
                            if (!fallback_valid || (current_is_amd_family && !fallback_is_amd_family)) {
                                fallback_valid = true;
                                fallback_bus = smbbus;
                                fallback_dev = smbdev;
                                fallback_fun = smbfun;
                                fallback_vid = vid;
                                fallback_did = did;
                            }
                            continue;
                        }
                    }
                }
            }
        }
    }
    if (fallback_valid) {
        smbbus = fallback_bus;
        smbdev = fallback_dev;
        smbfun = fallback_fun;
        return find_smb_controller(fallback_vid, fallback_did);
    }
    smbus_id = 0;
    return false;
}

// ----------------------------------------------------------
// WARNING: Be careful when adding a controller ID!
// Incorrect SMB accesses (ie: on bank switch) can brick your
// motherboard or your memory module.
//                           ----
// No Pull Request including a new SMBUS Controller will be
// accepted without a proof (screenshot) that it has been
// tested successfully on a real motherboard.
// ----------------------------------------------------------

// PCI device IDs for Intel i801 SMBus controller.
static const uint16_t intel_ich5_dids[] =
{
    0x2413,  // 82801AA (ICH)
    0x2423,  // 82801AB (ICH)
    0x2443,  // 82801BA (ICH2)
    0x2483,  // 82801CA (ICH3)
    0x24C3,  // 82801DB (ICH4)
    0x24D3,  // 82801E (ICH5)
    0x25A4,  // 6300ESB
    0x266A,  // 82801F (ICH6)
    0x269B,  // 6310ESB/6320ESB
    0x27DA,  // 82801G (ICH7)
    0x283E,  // 82801H (ICH8)
    0x2930,  // 82801I (ICH9)
    0x5032,  // EP80579 (Tolapai)
    0x3A30,  // ICH10
    0x3A60,  // ICH10
    0x3B30,  // 5/3400 Series (PCH)
    0x1C22,  // 6 Series (PCH)
    0x1D22,  // Patsburg (PCH)
    0x1D70,  // Patsburg (PCH) IDF
    0x1D71,  // Patsburg (PCH) IDF
    0x1D72,  // Patsburg (PCH) IDF
    0x2330,  // DH89xxCC (PCH)
    0x1E22,  // Panther Point (PCH)
    0x8C22,  // Lynx Point (PCH)
    0x9C22,  // Lynx Point-LP (PCH)
    0x1F3C,  // Avoton (SOC)
    0x8D22,  // Wellsburg (PCH)
    0x8D7D,  // Wellsburg (PCH) MS
    0x8D7E,  // Wellsburg (PCH) MS
    0x8D7F,  // Wellsburg (PCH) MS
    0x23B0,  // Coleto Creek (PCH)
    0x8CA2,  // Wildcat Point (PCH)
    0x9CA2,  // Wildcat Point-LP (PCH)
    0x0F12,  // BayTrail (SOC)
    0x2292,  // Braswell (SOC)
    0xA123,  // Sunrise Point-H (PCH)
    0x9D23,  // Sunrise Point-LP (PCH)
    0x19DF,  // Denverton  (SOC)
    0x1BC9,  // Emmitsburg (PCH)
    0xA1A3,  // Lewisburg (PCH)
    0xA223,  // Lewisburg Super (PCH)
    0xA2A3,  // Kaby Lake (PCH-H)
    0x31D4,  // Gemini Lake (SOC)
    0xA323,  // Cannon Lake-H (PCH)
    0x9DA3,  // Cannon Lake-LP (PCH)
    0x18DF,  // Cedar Fork (PCH)
    0x34A3,  // Ice Lake-LP (PCH)
    0x38A3,  // Ice Lake-N (PCH)
    0x02A3,  // Comet Lake (PCH)
    0x06A3,  // Comet Lake-H (PCH)
    0x4B23,  // Elkhart Lake (PCH)
    0xA0A3,  // Tiger Lake-LP (PCH)
    0x43A3,  // Tiger Lake-H (PCH)
    0x4DA3,  // Jasper Lake (SOC)
    0xA3A3,  // Comet Lake-V (PCH)
    0x7AA3,  // Alder Lake-S (PCH)
    0x51A3,  // Alder Lake-P (PCH)
    0x54A3,  // Alder Lake-M (PCH)
    0x7A23,  // Raptor Lake-S (PCH)
    0x7E22,  // Meteor Lake-P (SOC)
    //0xAE22,  // Meteor Lake-S (PCH)
    0x7F23,  // Arrow Lake-S (PCH)
    //0x5796,  // Birch Stream (SOC)
    0x7722,  // Arrow Lake-H (SOC)
    //0xA822,  // Lunar Lake
    0xE322,  // Panther Lake-H (SOC)
    //0xE422,  // Panther Lake-P (SOC)
};

static bool find_in_did_array(uint16_t did, const uint16_t * ids, unsigned int size)
{
    for (unsigned int i = 0; i < size; i++) {
        if (*ids++ == did) {
            return true;
        }
    }
    return false;
}

static bool is_smbus_pci_class(void)
{
    uint8_t base_class = pci_config_read8(smbbus, smbdev, smbfun, 0x0B);
    uint8_t sub_class = pci_config_read8(smbbus, smbdev, smbfun, 0x0A);
    return (base_class == 0x0C && sub_class == 0x05);
}
static bool find_smb_controller(uint16_t vid, uint16_t did)
{
    smbus_id = (((uint32_t)vid) << 16) | did;

    switch(vid)
    {
        case PCI_VID_INTEL:
        {
            if (find_in_did_array(did, intel_ich5_dids, ARRAY_SIZE(intel_ich5_dids)) || is_smbus_pci_class()) {
                return ich5_get_smb();
            }
            // if (did == 0x7113) { // 82371AB/EB/MB PIIX4
            //     return piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT);
            // }
            // 0x719B 82440/82443MX PMC - PIIX4
            // 0x0F13 ValleyView SMBus Controller ?
            // 0x8119 US15W ?
            return false;
        }

        case PCI_VID_HYGON:
        case PCI_VID_AMD:
            switch(did)
            {
                // case 0x740B: // AMD756
                // case 0x7413: // AMD766
                // case 0x7443: // AMD768
                // case 0x746B: // AMD8111_SMBUS
                // case 0x746A: // AMD8111_SMBUS2
                case 0x780B: // AMD FCH (Pre-Zen)
                    return amd_sb_get_smb();
                case 0x790B: // AMD FCH (Zen 2/3)
                    return fch_zen_get_smb();
                default:
                return false;
                if (!is_smbus_pci_class()) {
                        return false;
                    }
                    if (fch_zen_get_smb()) {
                        return true;
                    }
                    return amd_sb_get_smb();
            }
            break;

        case PCI_VID_ATI:
            switch(did)
            {
                // case 0x4353: // SB200
                // case 0x4363: // SB300
                case 0x4372:    // SB400
                    return piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT);
                case 0x4385:    // SB600+
                    return amd_sb_get_smb();
                default:
                    return false;
            }
            break;

        case PCI_VID_NVIDIA:
            switch(did)
            {
                // case 0x01B4: // nForce
                case 0x0064:    // nForce 2
                // case 0x0084: // nForce 2 Mobile
                case 0x00E4:    // nForce 3
                // case 0x0034: // MCP04
                // case 0x0052: // nForce 4
                case 0x0264:    // nForce 410/430 MCP
                case 0x03EB:    // nForce 630a
                // case 0x0446: // nForce 520
                // case 0x0542: // nForce 560
                case 0x0752:    // nForce 720a
                // case 0x07D8: // nForce 630i
                // case 0x0AA2: // nForce 730i
                // case 0x0D79: // MCP89
                case 0x0368:    // nForce 680a/680i/780i/790i
                    return nv_mcp_get_smb();
                default:
                    return false;
            }
            break;

        case PCI_VID_SIS:
            switch(did)
            {
                // case 0x0008:
                    // SiS5595, SiS630 or other SMBus controllers - it's complicated.
                // case 0x0016:
                    // SiS961/2/3, known as "SiS96x" SMBus controllers.
                // case 0x0018:
                // case 0x0964:
                    // SiS630 SMBus controllers.
                default:
                    return false;
            }
            break;

        case PCI_VID_VIA:
            switch(did)
            {
                // case 0x3040: // 82C586_3
                    // via SMBus controller.
                // case 0x3050: // 82C596_3
                    // Try SMB base address = 0x90, then SMB base address = 0x80
                    // viapro SMBus controller, i.e. PIIX4 with a small quirk.
                // case 0x3051: // 82C596B_3
                case 0x3057: // 82C686_4
                // case 0x8235: // 8231_4
                    // SMB base address = 0x90
                    // viapro SMBus controller, i.e. PIIX4.
                    return piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT);
                case 0x3074: // 8233
                case 0x3147: // 8233A
                case 0x3177: // 8235
                case 0x3227: // 8237
                // case 0x3337: // 8237A
                case 0x3372: // 8237S
                // case 0x3287: // 8251
                // case 0x8324: // CX700
                // case 0x8353: // VX800
                // case 0x8409: // VX855
                // case 0x8410: // VX900
                    // SMB base address = 0xD0
                    // viapro I2C controller, i.e. PIIX4 with a small quirk.
                    return piix4_get_smb(PIIX4_SMB_BASE_ADR_VIAPRO);
                default:
                    return false;
            }
            break;

        case PCI_VID_EFAR:
            switch(did)
            {
                // case 0x9463: // SLC90E66_3: PIIX4
                default:
                    return false;
            }
            break;

        case PCI_VID_ALI:
            switch(did)
            {
                case 0x7101: // ALi M1533/1535/1543C
                    return ali_get_smb(PIIX4_SMB_BASE_ADR_ALI1543);
                case 0x1563: // ALi M1563
                    return piix4_get_smb(PIIX4_SMB_BASE_ADR_ALI1563);
                default:
                    return false;
            }
            break;

        case PCI_VID_SERVERWORKS:
            switch(did)
            {
                case 0x0201: // CSB5
                    // From Linux i2c-piix4 driver: unlike its siblings, this model needs a quirk.
                    extra_initial_sleep_for_smb_transaction = 2100 - 500;
                // Fall through.
                // case 0x0200: // OSB4
                // case 0x0203: // CSB6
                // case 0x0205: // HT1000SB
                // case 0x0408: // HT1100LD
                    return piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT);
                default:
                    return false;
            }
            break;
        default:
            return false;
    }
    return false;
}

// ----------------------
// PIIX4 SMBUS Controller
// ----------------------

static bool piix4_get_smb(uint8_t address)
{
    uint16_t x = pci_config_read16(smbbus, smbdev, smbfun, address);

    if ((x & 0x0001) == 0 || (x & 0xFFF0) == 0) {
        return false;
    }

    {
        uint16_t base = x & 0xFFF0;
        smbusbase = base;
        return true;
    }

    // return false;
}

// ----------------------------
// i801 / ICH5 SMBUS Controller
// ----------------------------

static bool ich5_get_smb(void)
{
    uint16_t x;

    // Enable SMBus IO Space if disabled
    x = pci_config_read16(smbbus, smbdev, smbfun, 0x4);

    if (!(x & 1)) {
        pci_config_write16(smbbus, smbdev, smbfun, 0x4, x | 1);
    }

    // Read Base Address
    x = pci_config_read16(smbbus, smbdev, smbfun, 0x20);
        if ((x & 0x0001) == 0 || (x & 0xFFF0) == 0) {
        return false;
    }
    smbusbase = x & 0xFFF0;

    // Enable I2C Host Controller Interface if disabled
    // Use SMBUS Mode for DDR5 to allow bank switch using Proc Call
    // Disable SMI on SMBus
    uint8_t temp = pci_config_read8(smbbus, smbdev, smbfun, 0x40);
     uint8_t new_temp = temp | 0x01; // Enable Host
    new_temp &= (uint8_t)~0x02; // Disable SMI
    if (dmi_memory_device == NULL || dmi_memory_device->type != DMI_DDR5) {
        new_temp |= 0x04;
    }
    if (new_temp != temp) {
        pci_config_write8(smbbus, smbdev, smbfun, 0x40, new_temp);
    }
    
    // Disable Alert on LAN and other SMBus interrupters
    pci_config_write8(smbbus, smbdev, smbfun, 0x11, 0x00);
    
    // Clear INUSE first before anything else
    __outb(SMBHSTSTS_INUSE_STS, smbusbase + 0); // SMBHSTSTS
    usleep(5000);

    // Reset SMBUS Controller
    __outb(__inb(SMBHSTSTS) & 0x1F, SMBHSTSTS);
    usleep(500); 
    usleep(1000);

    return (smbusbase != 0);
}

// --------------------
// AMD SMBUS Controller
// --------------------

static bool amd_sb_get_smb(void)
{
    uint8_t rev_id;
    uint16_t pm_reg;

    rev_id = pci_config_read8(smbbus, smbdev, smbfun, 0x08);

    if ((smbus_id & 0xFFFF) == 0x4385 && rev_id <= 0x3D) {
        // Older AMD SouthBridge (SB700 & older) use PIIX4 registers
        if (!piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT)) {
            return false;
        }
        enable_smbus_io_decode();
        reset_smbus_host_controller();
        return true;
    } else if ((smbus_id & 0xFFFF) == 0x780B && rev_id == 0x42) {
        // Latest Pre-Zen APUs use the newer Zen PM registers
        return fch_zen_get_smb();
    } else {
         // AMD SB (SB800 up to pre-FT3/FP4/AM4) uses specific registers
        __outb(AMD_SMBUS_BASE_REG + 1, AMD_INDEX_IO_PORT);
        pm_reg = __inb(AMD_DATA_IO_PORT) << 8;
        __outb(AMD_SMBUS_BASE_REG, AMD_INDEX_IO_PORT);
        pm_reg |= __inb(AMD_DATA_IO_PORT) & 0xE0;

        if (pm_reg != 0xFFE0 && pm_reg != 0) {
            smbusbase = pm_reg;
            enable_smbus_io_decode();
            reset_smbus_host_controller();
            return true;
        }
    }
    if (piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT)) {
        enable_smbus_io_decode();
        reset_smbus_host_controller();
        return true;
    }

    return false;
}

static bool fch_zen_get_smb(void)
{
    uint16_t pm_reg;

    __outb(AMD_PM_INDEX + 1, AMD_INDEX_IO_PORT);
    pm_reg = __inb(AMD_DATA_IO_PORT) << 8;
    __outb(AMD_PM_INDEX, AMD_INDEX_IO_PORT);
    pm_reg |= __inb(AMD_DATA_IO_PORT);

    // Special case for AMD Family 19h & Extended Model > 4 (get smb address in memory)
    if ((imc.family == IMC_K19_CZN || imc.family == IMC_K19_PHX || imc.family == IMC_K19_RPL || imc.family >= IMC_K1A_STP) && pm_reg == 0xFFFF) {
        smbusbase = ((*(const uint32_t *)(0xFED80000 + 0x300) >> 8) & 0xFF) << 8;
        enable_smbus_io_decode();
        reset_smbus_host_controller();
        return true;
    }

    // Check if IO Smbus is enabled.
    if ((pm_reg & 0x10) == 0) {
        return false;
    }

    if ((pm_reg & 0xFF00) != 0) {
        smbusbase = pm_reg & 0xFF00;
        enable_smbus_io_decode();
        reset_smbus_host_controller();
        return true;
    }

    if (piix4_get_smb(PIIX4_SMB_BASE_ADR_DEFAULT)) {
        enable_smbus_io_decode();
        reset_smbus_host_controller();
        return true;
    }

    return false;
}

// -----------------------
// nVidia SMBUS Controller
// -----------------------

static bool nv_mcp_get_smb(void)
{
    int smbus_base_adr;

    if ((smbus_id & 0xFFFF) >= 0x200) {
        smbus_base_adr = NV_SMBUS_ADR_REG;
    } else {
        smbus_base_adr = NV_OLD_SMBUS_ADR_REG;
    }

    // nForce SB has 2 I2C Busses. SPD is located on first I2C Bus.
    uint16_t x = pci_config_read16(smbbus, smbdev, smbfun, smbus_base_adr) & 0xFFFC;

    if (x != 0) {
        smbusbase = x;
        return true;
    }

    return false;
}

// ---------------------------------------
// ALi SMBUS Controller (M1533/1535/1543C)
// ---------------------------------------

static bool ali_get_smb(uint8_t address)
{
    // Enable SMB I/O Base Address Register Control (Reg0x5B[2] = 0)
    uint16_t temp = pci_config_read8(smbbus, smbdev, smbfun, 0x5B);
    pci_config_write8(smbbus, smbdev, smbfun, 0x5B, temp & ~0x06);

    // Enable Response to I/O Access. (Reg0x04[0] = 1)
    temp = pci_config_read8(smbbus, smbdev, smbfun, 0x04);
    pci_config_write8(smbbus, smbdev, smbfun, 0x04, temp | 0x01);

    // SMB Host Controller Interface Enable (Reg0xE0[0] = 1)
    temp = pci_config_read8(smbbus, smbdev, smbfun, 0xE0);
    pci_config_write8(smbbus, smbdev, smbfun, 0xE0, temp | 0x01);

    // Read SMBase Register (usually 0xE800)
    uint16_t x = pci_config_read16(smbbus, smbdev, smbfun, address) & 0xFFF0;

    if (x != 0) {
        smbusbase = x;
        return true;
    }

    return false;
}

// --------------------
//  SPD Read functions
// --------------------

uint8_t get_spd(uint8_t slot_idx, uint16_t spd_adr)
{
    switch ((smbus_id >> 16) & 0xFFFF) {
      case PCI_VID_ALI:
        if ((smbus_id & 0xFFFF) == 0x7101)
            return ali_m1543_read_spd_byte(slot_idx, (uint8_t)spd_adr);
        else
            return ali_m1563_read_spd_byte(slot_idx, (uint8_t)spd_adr);
      case PCI_VID_NVIDIA:
        return nf_read_spd_byte(slot_idx, (uint8_t)spd_adr);
      case PCI_VID_AMD:
      case PCI_VID_HYGON:
        return amd_spd_read_byte(slot_idx, spd_adr);
      default:
        return ich5_read_spd_byte(slot_idx, spd_adr);
    }
}

uint8_t get_spd_hub_register(uint8_t slot_idx, uint8_t spd_hub_adr)
{
    if(dmi_memory_device_type == DMI_DDR5) {
        return ich5_read_spd_byte(slot_idx, spd_hub_adr | 0xFF00);
    }

    return 0;
}

/*************************************************************************************
/ *****************************         WARNING          *****************************
/ ************************************************************************************
/   Be absolutely sure to know what you're doing before changing the function below!
/       You can easily WRITE into the SPD (especially with DDR5) and corrupt it
/                /!\  Your RAM modules will not work anymore  /!\
/ *************************************************************************************/

static uint8_t ich5_read_spd_byte(uint8_t smbus_adr, uint16_t spd_adr)
{
    static uint8_t last_adr = 0xFF;
    static uint8_t spd_page = 0xFF;

    smbus_adr += 0x50; // standard I2C address for SPD

    // Some C612/server boards remap SPD to other address ranges like 0x50-0x53, or even 0x54-0x57.
    // X99/C612 Chinese boards sometimes map slots to 0x50, 0x52, 0x54, 0x56
    // We will just pass the slot_idx as standard, but we'll try a fallback scan if we fail.
    
    last_smb_addr = smbus_adr;
    last_smb_cmd = spd_adr;

    if (dmi_memory_device_type == DMI_DDR4) {
        // Switch page if needed (DDR4)
        if (spd_adr > 0xFF && spd_page != 1) {
            __outb((0x37 << 1) | I2C_WRITE, SMBHSTADD);
            __outb(SMBHSTCNT_BYTE_DATA, SMBHSTCNT);

            ich5_process(); // return should be 0x42 or 0x44
            spd_page = 1;

        } else if (spd_adr <= 0xFF && spd_page != 0) {
            __outb((0x36 << 1) | I2C_WRITE, SMBHSTADD);
            __outb(SMBHSTCNT_BYTE_DATA, SMBHSTCNT);

            ich5_process();
            spd_page = 0;
        }

        if (spd_adr > 0xFF) {
            spd_adr -= 0x100;
        }
    } else if (dmi_memory_device_type == DMI_DDR5) {
            // For DDR5, choose between reading from the SPD EEPROM (which may require a bank switch)
            // and reading from the DDR5 SPD Hub Register (where we added a 0xFF00 offset).

        if (spd_adr >> 8 != 0xFF) {
            // DDR5 SPD Read (7-bit address translated to  0x80-0xFF)
            // Switch page if needed (pages are 7-bit/128 bytes wide)
            uint8_t adr_page = spd_adr / 128;

            if (adr_page != spd_page || last_adr != smbus_adr) {

                // DDR5 SPD Bank switch can be achieved using 2 methods
                if(((smbus_id >> 16) & 0xFFFF) == PCI_VID_INTEL) {
                    // On Intel, we use the process call method because the SMBUS write command
                    // is sometimes disabled by BIOS to avoid unexpected SPD corruption
                    __outb((smbus_adr << 1) | I2C_READ, SMBHSTADD);
                    __outb(SPD5_HUB_I2C_CONF & 0x7F, SMBHSTCMD);
                    __outb(adr_page & 7, SMBHSTDAT0);
                    __outb(0, SMBHSTDAT1);
                    __outb(SMBHSTCNT_PROC_CALL, SMBHSTCNT);

                     ich5_process();

                    // These dummy read are mandatory to terminate a Proc Call
                    __inb(SMBHSTDAT0);
                    __inb(SMBHSTDAT1);

                } else {
                    // On AMD, we continue to use the standard smbus write command as it seems
                    // more reliable than the process call method. This may be reevaluated later.
                    __outb((smbus_adr << 1) | I2C_WRITE, SMBHSTADD);
                    __outb(SPD5_HUB_I2C_CONF & 0x7F, SMBHSTCMD);
                    __outb(adr_page & 7, SMBHSTDAT0);
                    __outb(SMBHSTCNT_BYTE_DATA, SMBHSTCNT);

                    ich5_process();
                }

                spd_page = adr_page;
                last_adr = smbus_adr;
             }

             // Get final I2C byte address on the current page (0x00-0x7F -> 0x80-0xFF).
             spd_adr -= adr_page * 128;
             spd_adr |= 0x80;

        } else {
            // SPD5 Hub Register Read (7-bit address at 0x00 - 0x7F)
            spd_adr &= 0x7F;
        }
    }

    // Standard read
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        __outb((smbus_adr << 1) | I2C_READ, SMBHSTADD);
        __outb(spd_adr, SMBHSTCMD);
        __outb(SMBHSTCNT_BYTE_DATA, SMBHSTCNT);

    uint8_t rc = ich5_process();
        last_smb_rc = rc;
        if (rc == 0) {
            return __inb(SMBHSTDAT0);
        }
        
        // Timeout wait to allow the controller to clear its bus collision state.
        usleep(10000);
        // Force reset the host controller between retries
        __outb(__inb(SMBHSTSTS) & 0x1F, SMBHSTSTS);
    }
    
    return 0xFF;
}

static uint8_t ich5_process(void)
{
    uint8_t status;
    uint16_t timeout = 0;

    status = __inb(SMBHSTSTS) & 0x1F;

    if (status != 0x00) {
        __outb(status, SMBHSTSTS);
        usleep(500);
        if ((status = (0x1F & __inb(SMBHSTSTS))) != 0x00) {
            return 1;
        }
    }

    __outb(__inb(SMBHSTCNT) | SMBHSTCNT_START, SMBHSTCNT);

    // Some SMB controllers need this quirk.
    if (extra_initial_sleep_for_smb_transaction) {
        usleep(extra_initial_sleep_for_smb_transaction);
    }

    do {
        usleep(500);
        status = __inb(SMBHSTSTS);
    } while ((status & 0x01) && (timeout++ < 200));
    
    last_smb_status = status;

    if (timeout >= 200) {
        return 2;
    }

    if (status & 0x1C) {
        return status & 0x1F;
    }

    return 0;
}

static uint8_t nf_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr)
{
    int i;

    smbus_adr += 0x50;

    // Set Slave ADR
    __outb(smbus_adr << 1, NVSMBADD);

    // Set Command (SPD Byte to Read)
    __outb(spd_adr, NVSMBCMD);

    // Start transaction
    __outb(NVSMBCNT_BYTE_DATA | NVSMBCNT_READ, NVSMBCNT);

    // Wait until transaction complete
    for (i = 500; i > 0; i--) {
        usleep(50);
        if (__inb(NVSMBCNT) == 0) {
            break;
        }
    }

    // If timeout or Error Status, quit
    if (i == 0 || __inb(NVSMBSTS) & NVSMBSTS_STATUS) {
        return 0xFF;
    }

    return __inb(NVSMBDAT(0));
}

static uint8_t ali_m1563_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr)
{
    int i;

    smbus_adr += 0x50;

    // Reset Status Register
     __outb(0xFF, SMBHSTSTS);

    // Set Slave ADR
    __outb((smbus_adr << 1 | I2C_READ), SMBHSTADD);

    __outb((__inb(SMBHSTCNT) & ~ALI_SMBHSTCNT_SIZEMASK) | (ALI_SMBHSTCNT_BYTE_DATA << 3), SMBHSTCNT);

    // Set Command (SPD Byte to Read)
    __outb(spd_adr, SMBHSTCMD);

    // Start transaction
    __outb(__inb(SMBHSTCNT) | SMBHSTCNT_START, SMBHSTCNT);

    // Wait until transaction complete
    for (i = 500; i > 0; i--) {
        usleep(50);
        if (!(__inb(SMBHSTSTS) & SMBHSTSTS_HOST_BUSY)) {
            break;
        }
    }
    // If timeout or Error Status, exit
    if (i == 0 || __inb(SMBHSTSTS) & ALI_SMBHSTSTS_BAD) {
        return 0xFF;
    }

    return __inb(SMBHSTDAT0);
}
static uint8_t amd_spd_read_byte(uint8_t slot_idx, uint16_t spd_adr)
{
    uint8_t smbus_adr = 0x50 + slot_idx;
    uint8_t status;
    uint16_t timeout = 0;
    
    // For DDR5, need to handle page switching first
    if (dmi_memory_device->type == DMI_DDR5 && spd_adr < 0x8000) {
        uint8_t adr_page = spd_adr / 128;
        
        // Switch page using SMBus write (AMD method)
        __outb(0xFF, SMBHSTSTS);  // Clear status
        
        __outb((smbus_adr << 1) | I2C_WRITE, SMBHSTADD);
        __outb(SPD5_HUB_I2C_CONF & 0x7F, SMBHSTCMD);
        __outb(adr_page & 7, SMBHSTDAT0);
        __outb(SMBHSTCNT_BYTE_DATA | SMBHSTCNT_START, SMBHSTCNT);
        
        // Wait for completion
        timeout = 0;
        do {
            usleep(500);
            status = __inb(SMBHSTSTS);
        } while ((status & 0x01) && (timeout++ < 100));
        
        // Calculate final address
        spd_adr -= adr_page * 128;
        spd_adr |= 0x80;
    }
    
    // Clear status
    __outb(0xFF, SMBHSTSTS);
    usleep(100);
    
    // Set Slave Address (SPD device + read bit)
    __outb((smbus_adr << 1) | I2C_READ, SMBHSTADD);
    
    // Set Command (SPD byte address)
    __outb((uint8_t)spd_adr, SMBHSTCMD);
    
    // Start transaction
    __outb(SMBHSTCNT_BYTE_DATA | SMBHSTCNT_START, SMBHSTCNT);
    
    // Wait for completion
    timeout = 0;
    do {
        usleep(500);
        status = __inb(SMBHSTSTS);
    } while ((status & 0x01) && (timeout++ < 100));
    
    if (timeout >= 100 || (status & 0x1C)) {
        return 0xFF;
    }
    
    return __inb(SMBHSTDAT0);
}
// static uint8_t amd_spd_read_byte(uint8_t slot_idx, uint16_t spd_adr)
// {
//     uint8_t smbus_adr = 0x50 + slot_idx;
//     int i;
    
//     // Reset Status Register
//     __outb(0xFF, SMBHSTSTS);
    
//     // Set Slave Address (SPD device + read bit)
//     __outb((smbus_adr << 1) | I2C_READ, SMBHSTADD);
    
//     // Set Command (SPD byte address)
//     __outb((uint8_t)spd_adr, SMBHSTCMD);
    
//     // Start transaction
//     __outb(SMBHSTCNT_BYTE_DATA | SMBHSTCNT_START, SMBHSTCNT);
    
//     // Wait for completion
//     for (i = 0; i < 100; i++) {
//         uint8_t status = __inb(SMBHSTSTS);
//         if (status & SMBHSTSTS_BYTE_DONE) {
//             __outb(status, SMBHSTSTS);  // Clear status
//             return __inb(SMBHSTDAT0);
//         }
//         usleep(100);
//     }
    
//     return 0xFF;
// }
static uint8_t ali_m1543_read_spd_byte(uint8_t smbus_adr, uint8_t spd_adr)
{
    int i;

    smbus_adr += 0x50;

    // Reset Status Register
     __outb(0xFF, SMBHSTSTS);

    // Set Slave ADR
    __outb((smbus_adr << 1 | I2C_READ), ALI_OLD_SMBHSTADD);

    // Set Command (SPD Byte to Read)
    __outb(spd_adr, ALI_OLD_SMBHSTCMD);

    // Start transaction
    __outb(ALI_OLD_SMBHSTCNT_BYTE_DATA, ALI_OLD_SMBHSTCNT);
    __outb(0xFF, ALI_OLD_SMBHSTSTART);

    // Wait until transaction complete
    for (i = 500; i > 0; i--) {
        usleep(50);
        if (!(__inb(SMBHSTSTS) & ALI_OLD_SMBHSTSTS_BUSY)) {
            break;
        }
    }

    // If timeout or Error Status, exit
    if (i == 0 || __inb(SMBHSTSTS) & ALI_OLD_SMBHSTSTS_BAD) {
        return 0xFF;
    }

    return __inb(ALI_OLD_SMBHSTDAT0);
}

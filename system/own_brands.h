/* Our own memory module brands (shown with a gold thumbs-up on screen).
 *
 * Usage: just add one line per brand, e.g.
 *     OWN_BRAND(0x1234, "MYBRAND")
 *
 * The ID is the JEDEC manufacturer ID. Do NOT bother with parity bits
 * or byte order - write the number exactly as your SPD tool displays
 * it, both notations work:
 *     0x5E10 (ID 0x5E, bank 0x10)  == 0x105E
 *     0x9168 (raw bytes, parity set) == 0x1168
 *
 * The name is also used as a case-insensitive keyword to recognise the
 * module from its part number when no SPD can be read (SMBIOS
 * fallback), so it should appear in the part number you program.
 *
 * Note: this table only drives the on-screen display. If you also want
 * the brand name in the generated reports, add a matching ENTRY() in
 * jedec_id.h as well.
 */

OWN_BRAND(0x5E10, "HEROSYS")      // == 0x105E
OWN_BRAND(0x9168, "ZhaoChu")      // == 0x1168
OWN_BRAND(0x0E9D, "KINSOTIN")     // == 0x0E1D
OWN_BRAND(0xA88A, "XiShenHua")    // == 0x080A
OWN_BRAND(0x9192, "Huazhixincun") // == 0x1112
OWN_BRAND(0x89F7, "Netac Technology Co Ltd") // == 0x0977
OWN_BRAND(0x91DF, "Shanghai RORKE Storage") // == 0x115F

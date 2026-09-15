// SPDX-License-Identifier: GPL-2.0
#ifndef SCREEN_H
#define SCREEN_H
/**
 * \file
 *
 * Provides the display interface. It provides an 80x25 VGA-compatible text
 * display.
 *
 *//*
 * Copyright (C) 2020-2024 Martin Whitaker.
 */

#include <stdint.h>

/**
 * Screen size definitions. The screen size cannot be changed.
 */
#define SCREEN_WIDTH    80
#define SCREEN_HEIGHT   25

typedef union {
    struct {
        uint8_t     ch;
        uint8_t     attr;
    };
    struct {
        uint16_t    value;
    };
} vga_char_t;

typedef vga_char_t vga_buffer_t[SCREEN_HEIGHT][SCREEN_WIDTH];

/*
 * Glyph identifiers stored in the shadow buffer.
 *  - 0 .. 255          : 8x16 glyphs from font_data (ASCII / CP437)
 *  - 256 .. 0xFFFE     : 16x16 CJK glyphs from cn_font_data (id - 256)
 *  - 0xFFFF            : continuation cell (right half of a CJK glyph)
 */
#define GLYPH_CN_BASE   256
#define GLYPH_CONT      0xFFFF

typedef struct {
    uint16_t    id;
    uint8_t     attr;
} shadow_char_t;

/**
 * Colours that can be used for the foreground or background.
 */
typedef enum {
    BLACK       = 0,
    BLUE        = 1,
    GREEN       = 2,
    CYAN        = 3,
    RED         = 4,
    MAUVE       = 5,
    YELLOW      = 6,
    WHITE       = 7
} screen_colour_t;

/**
 * Colour Palette definition
 */
typedef struct {
    screen_colour_t background;
    screen_colour_t foreground;
    screen_colour_t title_background;
    screen_colour_t title_foreground;
    screen_colour_t footer_background;
    screen_colour_t footer_foreground;
    screen_colour_t popup_background;
} screen_palette_t;

/**
 * BIOS/UEFI(GOP) agnostic framebuffer copy
 */
extern shadow_char_t shadow_buffer[SCREEN_HEIGHT][SCREEN_WIDTH];

/**
 * Modifier that can be added to any foreground colour.
 * Has no effect on background colours.
 */
#define BOLD        8

/**
 * Initialise the display interface.
 */
void screen_init(void);

/**
 * Set the foreground colour used for subsequent drawing operations.
 */
void set_foreground_colour(screen_colour_t colour);

/**
 * Set the background colour used for subsequent drawing operations.
 */
void set_background_colour(screen_colour_t colour);

/**
 * Clear the whole screen, using the current background colour.
 */
void clear_screen(void);

/**
 * Clear the specified region of the screen, using the current background
 * colour.
 */
void clear_screen_region(int start_row, int start_col, int end_row, int end_col);

/**
 * Move the contents of the specified region of the screen up one row,
 * discarding the top row, and clearing the bottom row, using the current
 * background colour.
 */
void scroll_screen_region(int start_row, int start_col, int end_row, int end_col);

/**
 * Copy the contents of the specified region of the screen into the supplied
 * buffer.
 */
void save_screen_region(int start_row, int start_col, int end_row, int end_col, shadow_char_t buffer[]);

/**
 * Restore the specified region of the screen from the supplied buffer.
 * This restores both text and colours.
 */
void restore_screen_region(int start_row, int start_col, int end_row, int end_col, const shadow_char_t buffer[]);

/**
 * Write the supplied character (glyph id 0..255) to the specified screen
 * location, using the current foreground colour. Has no effect if the
 * location is outside the screen.
 */
void print_char(int row, int col, int ch);

/**
 * Write the supplied Unicode code point to the specified screen location.
 * CJK code points use a 16x16 glyph that occupies two character cells.
 * Returns the column just past the character (col + display width).
 */
int print_codepoint(int row, int col, uint32_t codepoint);

#endif // SCREEN_H

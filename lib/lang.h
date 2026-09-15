// SPDX-License-Identifier: GPL-2.0
#ifndef LANG_H
#define LANG_H
/**
 * \file
 *
 * Runtime UI language selection (Chinese / English).
 *
 *//*
 * Copyright (C) 2026 HEROSYS.
 */

#include <stdbool.h>

/* Current UI language: true = Chinese (default), false = English. */
extern bool lang_cn;

/*
 * Returns the English replacement for a known Chinese string, or the
 * string itself when no translation is found. When Chinese is selected
 * the string is always returned unchanged (zero overhead).
 */
const char *lang_translate(const char *str);

#endif // LANG_H

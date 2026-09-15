// SPDX-License-Identifier: GPL-2.0
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lang.h"

//------------------------------------------------------------------------------
// Public Variables
//------------------------------------------------------------------------------

/* Current UI language: true = Chinese (default), false = English. */
bool lang_cn = true;

//------------------------------------------------------------------------------
// Private Variables
//------------------------------------------------------------------------------

/*
 * Central Chinese -> English translation table.
 *
 * Keys must match the string literals in the source code byte for byte,
 * including trailing spaces (they exist to overwrite leftover glyphs when
 * a shorter string replaces a longer one on screen).
 *
 * Display column layout of the fixed screen lines (see display.c) is
 * preserved: value fields (N/A, percentages, counters) keep the same
 * columns in both languages.
 */
typedef struct {
    const char *zh;
    const char *en;
} lang_entry_t;

static const lang_entry_t lang_table[] = {
    // ---- config.c : initial notice ----
    { "按 <F1> 配置，<F2> 禁用 SMP，<Enter> 开始测试",   "Press <F1> to configure, <F2> to disable SMP, <Enter> to start testing" },
    { "按 <F1> 配置，<F2> 启用 SMP，<Enter> 开始测试 ",  "Press <F1> to configure, <F2> to enable SMP, <Enter> to start testing " },

    // ---- config.c : test selection menu ----
    { "当前选择:",                                       "Current selection:" },
    { "当前序列:",                                       "Current sequence:" },
    { "  (滚动 U D)",                                    "  (scroll U D)" },
    { "测试选择:",                                       "Test Selection:" },
    { "<F1>  清除选择",                                  "<F1>  Clear selection" },
    { "<F2>  移除单个测试",                              "<F2>  Remove one test" },
    { "<F3>  添加单个测试",                              "<F3>  Add one test" },
    { "<F4>  添加测试范围",                              "<F4>  Add test range" },
    { "<F5>  添加全部测试",                              "<F5>  Add all tests" },
    { "<F6>  设置测试顺序",                              "<F6>  Set test sequence" },
    { "<F7>  测试循环次数 : %u      ",                   "<F7>  Test cycle times : %u      " },
    { "<F8>  出错时继续 : %s     ",                      "<F8>  Continue on error : %s     " },
    { "<F10> 退出菜单",                                  "<F10> Exit menu" },
    { "输入测试编号#",                                   "Enter test#" },
    { "无效的测试编号",                                  "Invalid test number" },
    { "输入序列 %u#",                                    "Seq %u#" },
    { "重复的测试编号",                                  "Duplicate test number" },
    { "输入起始测试编号:",                               "First test #:" },
    { "输入结束测试编号:",                               "Last test #:" },
    { "无效的测试范围",                                  "Invalid test range" },
    { "输入测试循环次数:",                               "Test cycle times:" },
    { "必须至少选择一个测试",                            "Select at least one test" },
    { "是",                                              "Yes" },
    { "否",                                              "No " },

    // ---- config.c : address range menu ----
    { "地址范围:",                                       "Address Range:" },
    { "<F1>  设置下限",                                  "<F1>  Set lower limit" },
    { "<F2>  设置上限",                                  "<F2>  Set upper limit" },
    { "<F3>  测试全部内存",                              "<F3>  Test all memory" },
    { "当前范围: %kB - %kB",                             "Current range: %kB - %kB" },
    { "输入下限: ",                                      "Lower: " },
    { "下限必须小于上限",                                "Lower must be less than upper" },
    { "输入上限: ",                                      "Upper: " },
    { "上限必须大于下限",                                "Upper must be greater than lower" },

    // ---- config.c : cpu mode / error mode menus ----
    { "CPU 调度模式:",                                   "CPU Sequencing Mode:" },
    { "<F1>  并行    (PAR)",                             "<F1>  Parallel    (PAR)" },
    { "<F2>  串行  (SEQ)",                               "<F2>  Sequential  (SEQ)" },
    { "<F3>  轮询 (RR)",                                 "<F3>  Round robin (RR)" },
    { "错误报告模式:",                                   "Error Reporting Mode:" },
    { "<F1>  仅错误计数",                                "<F1>  Error counts only" },
    { "<F2>  错误摘要",                                  "<F2>  Error summary" },
    { "<F3>  逐条错误",                                  "<F3>  Individual errors" },
    { "<F4>  BadRAM 模式",                               "<F4>  BadRAM patterns" },
    { "<F5>  Linux 内存映射",                            "<F5>  Linux memmap" },
    { "<F6>  坏页列表",                                  "<F6>  Bad pages" },

    // ---- config.c : cpu selection menu ----
    { "CPU 选择:",                                       "CPU Selection:" },
    { "<F2>  移除单个CPU",                               "<F2>  Remove one CPU" },
    { "<F3>  添加单个CPU",                               "<F3>  Add one CPU" },
    { "<F4>  添加CPU范围",                               "<F4>  Add CPU range" },
    { "<F5>  添加全部CPU",                               "<F5>  Add all CPUs" },
    { "<F6>  包含能效核    ",                            "<F6>  Include E-Cores" },
    { "<F6>  排除能效核    ",                            "<F6>  Exclude E-Cores" },
    { "排除能效核    ",                                  "Exclude E-Cores" },
    { "包含能效核    ",                                  "Include E-Cores" },
    { "输入CPU编号#",                                    "Enter CPU #" },
    { "无效的CPU编号",                                   "Invalid CPU number" },
    { "输入起始CPU编号:",                                "First CPU #:" },
    { "输入结束CPU编号:",                                "Last CPU #:" },
    { "无效的CPU范围",                                   "Invalid CPU range" },

    // ---- config.c : boot options menu ----
    { "启动选项:",                                       "Boot options:" },
    { "<F1>  启动跟踪 %s",                               "<F1>  Boot trace %s" },
    { "<F2>  ECC 轮询 %s",                               "<F2>  ECC polling %s" },
    { "禁用",                                            "disable" },
    { "启用",                                            "enable " },

    // ---- config.c : main config menu ----
    { "设置:",                                           "Settings:" },
    { "<F1>  测试选择",                                  "<F1>  Test selection" },
    { "<F2>  地址范围",                                  "<F2>  Address range" },
    { "<F3>  CPU 调度模式",                              "<F3>  CPU sequencing mode" },
    { "<F4>  错误报告模式",                              "<F4>  Error reporting mode" },
    { "<F5>  CPU 选择",                                  "<F5>  CPU selection" },
    { "<F6>  CPU 温度 %s",                               "<F6>  CPU Temperature %s" },
    { "<F7>  内存温度 %s",                               "<F7>  RAM Temperature %s" },
    { "<F8>  启动选项",                                  "<F8>  Boot options" },
    { "< S > 保存配置到U盘",                             "< S > Save configuration to USB" },
    { "< D > 恢复默认配置",                              "< D > Restore default config" },
    { "<F5>  跳过当前测试",                              "<F5>  Skip current test" },
    { "<F6>  保存到 %s           ",                      "<F6>  Save to %s           " },
    { "<F6>  保存结果到U盘",                             "<F6>  Save results to USB" },
    { "正在保存配置...",                                 "Saving config..." },
    { "正在恢复默认设置...",                             "Restoring defaults..." },
    { "正在重启...",                                     "Rebooting..." },

    // ---- display.c : fixed screen lines ----
    { "时钟/温度: N/A              | 轮次   %",          "CLK/Temp:  N/A              | Pass   %" },
    { "L1 缓存:  N/A               | 测试   %",          "L1 Cache: N/A               | Test   %" },
    { "L2 缓存:  N/A               | 测试 #",            "L2 Cache: N/A               | Test #" },
    { "L3 缓存:  N/A               | 测试项:",           "L3 Cache: N/A               | Testing:" },
    { "内存容量: N/A               | 数据模式:",         "Memory  : N/A               | Pattern:" },
    { "轮次:          错误:                      |时间:           状态:  初始化", "Pass:           Err:                      |Time:           Status:Initial" },
    { "错误:       ECC:",                                "Err:        ECC:" },
    { " <ESC> 退出 <F1> 配置 <F2> 快速/完整 <F3> D3 <F4> D4 <F5> D5 <F11> 2-", " <ESC> Exit <F1> Configuration <F2> Quick/Full <F3> D3 <F4> D4 <F5> D5 <F11> 2-" },

    // ---- display.c / error.c / main.c : status & messages ----
    { "测试中",                                          "Testing" },
    { "通过  ",                                          "Pass   " },
    { "失败! ",                                          "Failed!" },
    { "按任意键关闭此提示 ",                             "Press any key to remove this banner " },
    { "<Enter> 单步执行     ",                           "<Enter> Single step     " },
    { "需要加密狗！等待中...",                            "Need Dongle! Waiting..." },
    { "检测到CPU栈溢出 - 测试结果不可靠",                "CPU stack overflow detected - test results unreliable" },

    // ---- tests.c : static test descriptions (trailing spaces kept) ----
    { "[地址测试，逐位走1，无缓存]",                     "[Address test, walking ones, no cache] " },
    { "[地址测试，窗口内自身地址] ",                     "[Address test, own address in window]  " },
    { "[地址测试，自身地址+窗口]   ",                    "[Address test, own address + window]   " },
    { "[总线压力，读写切换，随机] ",                     "[Bus stress, R/W turnaround, random]   " },
    { "[移动取反，全1和全0]           ",                 "[Moving inversions, 1s & 0s]           " },
    { "[移动取反，随机序列]          ",                  "[Moving inversions, random sequence]   " },
    { "[移动取反，8位模式]            ",                 "[Moving inversions, 8 bit pattern]     " },
    { "[取模20，随机模式]              ",                "[Modulo 20, random pattern]            " },
    { "[块移动]                            ",            "[Block move]                           " },
    { "[移动取反，64位模式]          ",                  "[Moving inversions, 64 bit pattern]    " },
    { "[移动取反，32位模式]          ",                  "[Moving inversions, 32 bit pattern]    " },
    { "[位衰减测试，0/1/随机]         ",                 "[Bit fade test, 0s, 1s, random]        " },
    { "[Rowhammer，Blacksmith式]            ",           "[Rowhammer, Blacksmith-style]          " },
};

//------------------------------------------------------------------------------
// Private Functions
//------------------------------------------------------------------------------

static bool streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

//------------------------------------------------------------------------------
// Public Functions
//------------------------------------------------------------------------------

const char *lang_translate(const char *str)
{
    if (lang_cn || str == NULL) {
        return str;
    }

    /* Fast path: skip pure-ASCII strings (formats, numbers, English text).
     * Many Chinese strings start with ASCII ("<F1>  ..."), so the whole
     * string must be scanned for any byte >= 0x80. */
    bool has_wide = false;
    for (const unsigned char *p = (const unsigned char *)str; *p != '\0'; p++) {
        if (*p >= 0x80) {
            has_wide = true;
            break;
        }
    }
    if (!has_wide) {
        return str;
    }

    for (size_t i = 0; i < sizeof(lang_table) / sizeof(lang_table[0]); i++) {
        if (streq(str, lang_table[i].zh)) {
            return lang_table[i].en;
        }
    }
    return str;
}

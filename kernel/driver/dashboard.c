/*
 * dashboard.c - Implementation of the system dashboard driver.
 *
 * This module handles the rendering of real-time system metrics such as
 * uptime, memory utilization, and CPU activity.
 */

#include "driver/dashboard.h"

#include "string.h"

#include "core/timer.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/slab.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "driver/graphics.h"
#include "driver/fb.h"

/*
 * uint_to_str - Converts an unsigned long to its decimal string representation.
 */
static void uint_to_str(unsigned long n, char *buf)
{
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return;
    }

    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    buf[i] = '\0';

    for (int j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - j - 1];
        buf[i - j - 1] = tmp;
    }
}

/*
 * dashboard_update - Refresh and render the system metrics bar.
 */
void dashboard_update(void)
{
    char buf[64];
    unsigned int x = 10;
    unsigned long uptime = get_system_time() / 1000;

    /* Background bar */
    graphics_draw_rect(0, 0, fb_info.width, 20, 0x00333333, 1);

    /* Uptime */
    uint_to_str(uptime, buf);
    graphics_draw_string(x, 6, buf, 0xFFFFFF00, 0xFFFFFFFF);
    x += (strlen(buf) * 8);
    graphics_draw_string(x, 6, "s", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8 * 4;

    /* Physical Memory Usage */
    graphics_draw_string(x, 6, "PMM:", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8 * 4;
    unsigned long p_free = pmm_get_free_pages();
    unsigned long p_total = pmm_get_total_pages();
    uint_to_str((p_total - p_free) * 4 / 1024, buf);
    graphics_draw_string(x, 6, buf, 0xFF00FF00, 0xFFFFFFFF);
    x += (strlen(buf) * 8);
    graphics_draw_string(x, 6, "/", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8;
    uint_to_str(p_total * 4 / 1024, buf);
    graphics_draw_string(x, 6, buf, 0xFFFFFFFF, 0xFFFFFFFF);
    x += (strlen(buf) * 8);
    graphics_draw_string(x, 6, "MB", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8 * 4;

    /* Kernel Heap Usage */
    graphics_draw_string(x, 6, "HEP:", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8 * 4;
    unsigned long h_used = heap_get_used();
    unsigned long h_total = heap_get_total();
    uint_to_str(h_used / 1024, buf);
    graphics_draw_string(x, 6, buf, 0xFF00FFFF, 0xFFFFFFFF);
    x += (strlen(buf) * 8);
    graphics_draw_string(x, 6, "/", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8;
    uint_to_str(h_total / 1024, buf);
    graphics_draw_string(x, 6, buf, 0xFFFFFFFF, 0xFFFFFFFF);
    x += (strlen(buf) * 8);
    graphics_draw_string(x, 6, "KB", 0xFFFFFFFF, 0xFFFFFFFF);
    x += 8 * 4;

    /* Multi-core Scheduling Status */
    for (int i = 0; i < 4; i++) {
        char c_label[4] = "C0:";
        c_label[1] = '0' + i;
        graphics_draw_string(x, 6, c_label, 0xFFAAAAAA, 0xFFFFFFFF);
        x += 8 * 3;

        int pid = sched_get_core_pid(i);
        if (pid <= 0) {
            graphics_draw_string(x, 6, "K", 0xFFFF0000, 0xFFFFFFFF);
        } else {
            uint_to_str((unsigned long)pid, buf);
            graphics_draw_string(x, 6, buf, 0xFFFFFFFF, 0xFFFFFFFF);
        }
        x += 8 * 3;
    }
}

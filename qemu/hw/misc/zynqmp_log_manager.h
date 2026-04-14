/*
 * ZynqMP custom logging manager for personalized QEMU devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ZYNQMP_LOG_MANAGER_H
#define HW_MISC_ZYNQMP_LOG_MANAGER_H

#include <stdarg.h>

typedef enum ZynqmpLogChannel {
    ZYNQMP_LOG_CHANNEL_PLL = 0,
    ZYNQMP_LOG_CHANNEL_DUMMY = 1,
    ZYNQMP_LOG_CHANNEL_MAX,
} ZynqmpLogChannel;

void zynqmp_log_manager_set_channel_path(ZynqmpLogChannel channel,
                                         const char *env_name,
                                         const char *default_path);
void zynqmp_log_printf(ZynqmpLogChannel channel, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void zynqmp_log_vprintf(ZynqmpLogChannel channel, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));
void zynqmp_log_exit(void);

#endif

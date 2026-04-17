/*
 * ZynqMP custom logging manager for personalized QEMU devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ZYNQMP_LOG_MANAGER_H
#define HW_MISC_ZYNQMP_LOG_MANAGER_H

typedef enum ZynqmpLogChannel {
    ZYNQMP_LOG_CHANNEL_PLL = 0,
    ZYNQMP_LOG_CHANNEL_DUMMY = 1,
    ZYNQMP_LOG_CHANNEL_MAX,
} ZynqmpLogChannel;

void zynqmp_log_manager_set_channel_path(ZynqmpLogChannel channel,
                                         const char *env_name,
                                         const char *default_path);
void custom_log(ZynqmpLogChannel channel_number, const char *msg_string);
void zynqmp_log_exit(void);

#endif

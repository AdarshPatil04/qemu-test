/*
 * ZynqMP custom logging manager for personalized QEMU devices.
 *
 * Centralized API with Linux message queue buffering and graceful close.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/zynqmp_log_manager.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define ZYNQMP_LOG_MAX_LINE 512

typedef struct ZynqmpLogMessage {
    long mtype;
    int channel_number;
    char msg_box[ZYNQMP_LOG_MAX_LINE];
} ZynqmpLogMessage;

typedef struct ZynqmpLogChannelState {
    FILE *fp;
    char path[256];
} ZynqmpLogChannelState;

static ZynqmpLogChannelState zynqmp_channels[ZYNQMP_LOG_CHANNEL_MAX];
static bool zynqmp_hooks_installed;
static int zynqmp_mqid = -1;
static QemuThread zynqmp_logger_thread;
static bool zynqmp_logger_started;
static volatile sig_atomic_t zynqmp_shutdown_requested;

static void zynqmp_log_manager_init_once(void);
static FILE *zynqmp_log_get_fp(ZynqmpLogChannel channel);
static void *zynqmp_logger_thread_main(void *opaque);
static void zynqmp_log_write_shutdown_marker(void);
static void zynqmp_log_close_all_files(void);
static const char *zynqmp_log_channel_name(ZynqmpLogChannel channel);

static void zynqmp_log_close_all_files(void)
{
    int i;

    for (i = 0; i < ZYNQMP_LOG_CHANNEL_MAX; i++) {
        if (zynqmp_channels[i].fp) {
            fflush(zynqmp_channels[i].fp);
            fclose(zynqmp_channels[i].fp);
            zynqmp_channels[i].fp = NULL;
        }
    }

    if (zynqmp_mqid != -1) {
        msgctl(zynqmp_mqid, IPC_RMID, NULL);
        zynqmp_mqid = -1;
    }
}

static const char *zynqmp_log_channel_name(ZynqmpLogChannel channel)
{
    switch (channel) {
    case ZYNQMP_LOG_CHANNEL_PLL:
        return "PLL";
    case ZYNQMP_LOG_CHANNEL_DUMMY:
        return "DUMMY";
    default:
        return "UNKNOWN";
    }
}

void zynqmp_log_exit(void)
{
    zynqmp_shutdown_requested = 1;

    if (zynqmp_mqid != -1) {
        msgctl(zynqmp_mqid, IPC_RMID, NULL);
        zynqmp_mqid = -1;
    }
}

static void zynqmp_log_manager_init_once(void)
{
    if (zynqmp_mqid == -1) {
        zynqmp_mqid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    }

    if (!zynqmp_hooks_installed) {
        atexit(zynqmp_log_exit);
        zynqmp_hooks_installed = true;
    }

    if (!zynqmp_logger_started) {
        qemu_thread_create(&zynqmp_logger_thread,
                           "zynqmp-logger",
                           zynqmp_logger_thread_main,
                           NULL,
                           QEMU_THREAD_DETACHED);
        zynqmp_logger_started = true;
    }
}

void zynqmp_log_manager_set_channel_path(ZynqmpLogChannel channel,
                                         const char *env_name,
                                         const char *default_path)
{
    const char *path;

    if (channel < 0 || channel >= ZYNQMP_LOG_CHANNEL_MAX || !default_path) {
        return;
    }

    path = env_name ? getenv(env_name) : NULL;
    if (!path || !path[0]) {
        path = default_path;
    }

    pstrcpy(zynqmp_channels[channel].path,
            sizeof(zynqmp_channels[channel].path),
            path);
}

static FILE *zynqmp_log_get_fp(ZynqmpLogChannel channel)
{
    ZynqmpLogChannelState *st;

    if (channel < 0 || channel >= ZYNQMP_LOG_CHANNEL_MAX) {
        return NULL;
    }

    st = &zynqmp_channels[channel];

    if (!st->fp) {
        if (!st->path[0]) {
            return NULL;
        }

        st->fp = fopen(st->path, "a");
        if (!st->fp) {
            return NULL;
        }

        setvbuf(st->fp, NULL, _IOLBF, 0);
    }

    return st->fp;
}

static void *zynqmp_logger_thread_main(void *opaque)
{
    ZynqmpLogMessage msg;

    while (1) {
        ssize_t rc;

        if (zynqmp_mqid == -1) {
            break;
        }

        rc = msgrcv(zynqmp_mqid, &msg, sizeof(msg) - sizeof(long), 0, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        FILE *fp = zynqmp_log_get_fp((ZynqmpLogChannel)msg.channel_number);
        if (fp) {
            fprintf(fp, "[channel=%d/%s] %s",
                    msg.channel_number,
                    zynqmp_log_channel_name((ZynqmpLogChannel)msg.channel_number),
                    msg.msg_box);
            fflush(fp);
        }
    }

    if (zynqmp_shutdown_requested) {
        zynqmp_log_write_shutdown_marker();
    }

    zynqmp_log_close_all_files();
    return NULL;
}

static void zynqmp_log_write_shutdown_marker(void)
{
    FILE *fp;
    int i;

    for (i = 0; i < ZYNQMP_LOG_CHANNEL_MAX; i++) {
        fp = zynqmp_channels[i].fp;
        if (fp) {
            fprintf(fp, "[LOG-MANAGER] Graceful shutdown (normal/signal exit)\n");
            fflush(fp);
        }
    }
}

void custom_log(ZynqmpLogChannel channel_number, const char *msg_string)
{
    ZynqmpLogMessage msg;

    if (!msg_string) {
        return;
    }

    zynqmp_log_manager_init_once();

    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.channel_number = (int)channel_number;

    pstrcpy(msg.msg_box, sizeof(msg.msg_box), msg_string);

    if (msgsnd(zynqmp_mqid, &msg, sizeof(msg) - sizeof(long), 0) < 0) {
        return;
    }
}

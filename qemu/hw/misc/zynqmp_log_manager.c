/*
 * ZynqMP custom logging manager for personalized QEMU devices.
 *
 * Centralized API with Linux message queue buffering and graceful close.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/zynqmp_log_manager.h"
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
    int channel;
    char text[ZYNQMP_LOG_MAX_LINE];
} ZynqmpLogMessage;

typedef struct ZynqmpLogChannelState {
    FILE *fp;
    char path[256];
} ZynqmpLogChannelState;

static ZynqmpLogChannelState zynqmp_channels[ZYNQMP_LOG_CHANNEL_MAX];
static bool zynqmp_hooks_installed;
static int zynqmp_mqid = -1;

static void zynqmp_log_manager_init_once(void);
static void zynqmp_log_drain_queue(void);
static FILE *zynqmp_log_get_fp(ZynqmpLogChannel channel);

static void zynqmp_log_close_all(void)
{
    int i;

    zynqmp_log_drain_queue();

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

static void zynqmp_log_signal_close(int signum)
{
    zynqmp_log_exit();
    signal(signum, SIG_DFL);
    raise(signum);
}

void zynqmp_log_exit(void)
{
    FILE *fp;

    fp = zynqmp_log_get_fp(ZYNQMP_LOG_CHANNEL_PLL);
    if (fp) {
        fprintf(fp, "[LOG-MANAGER] Graceful shutdown (normal/signal exit)\n");
        fflush(fp);
    }

    fp = zynqmp_log_get_fp(ZYNQMP_LOG_CHANNEL_DUMMY);
    if (fp) {
        fprintf(fp, "[LOG-MANAGER] Graceful shutdown (normal/signal exit)\n");
        fflush(fp);
    }

    zynqmp_log_close_all();
}

static void zynqmp_log_manager_init_once(void)
{
    if (zynqmp_mqid == -1) {
        zynqmp_mqid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    }

    if (!zynqmp_hooks_installed) {
        signal(SIGINT, zynqmp_log_signal_close);
        signal(SIGTERM, zynqmp_log_signal_close);
        signal(SIGQUIT, zynqmp_log_signal_close);
        signal(SIGHUP, zynqmp_log_signal_close);
        atexit(zynqmp_log_exit);
        zynqmp_hooks_installed = true;
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

static void zynqmp_log_drain_queue(void)
{
    ZynqmpLogMessage msg;

    if (zynqmp_mqid == -1) {
        return;
    }

    while (msgrcv(zynqmp_mqid, &msg, sizeof(msg) - sizeof(long), 0,
                  IPC_NOWAIT) >= 0) {
        FILE *fp = zynqmp_log_get_fp((ZynqmpLogChannel)msg.channel);
        if (fp) {
            fputs(msg.text, fp);
            fflush(fp);
        }
    }

    if (errno != ENOMSG) {
        errno = 0;
    }
}

void zynqmp_log_vprintf(ZynqmpLogChannel channel, const char *fmt, va_list ap)
{
    ZynqmpLogMessage msg;
    int rc;

    if (!fmt) {
        return;
    }

    zynqmp_log_manager_init_once();

    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.channel = (int)channel;

    rc = vsnprintf(msg.text, sizeof(msg.text), fmt, ap);
    if (rc < 0) {
        return;
    }

    if (msgsnd(zynqmp_mqid, &msg, sizeof(msg) - sizeof(long), IPC_NOWAIT) < 0) {
        return;
    }

    zynqmp_log_drain_queue();
}

void zynqmp_log_printf(ZynqmpLogChannel channel, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    zynqmp_log_vprintf(channel, fmt, ap);
    va_end(ap);
}

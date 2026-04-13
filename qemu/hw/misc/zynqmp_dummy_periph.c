/*
 * ZynqMP Dummy Peripheral for Custom QEMU
 *
 * FDT-compatible memory-mapped peripheral with:
 *   - File-based logging (append mode, no stderr)
 *   - Graceful shutdown via SIGINT/SIGTERM (fclose)
 *   - Read/write register storage
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/fdt_generic_util.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define TYPE_ZYNQMP_DUMMY "zynqmp.dummy-periph"
#define NUM_REGS 4
#define LOG_FILE_DEFAULT "/home/adarshpatil04/workspace/qemu-run/hooks.log"

static FILE *log_fp = NULL;
static bool log_hooks_installed;
static const char *dummy_log_path;

static const char *dummy_get_log_path(void)
{
    const char *path = getenv("QEMU_DUMMY_LOG_FILE");

    if (!path || !path[0]) {
        path = getenv("QEMU_LOG_FILE");
    }
    if (!path || !path[0]) {
        path = LOG_FILE_DEFAULT;
    }
    return path;
}

static void dummy_log_atexit(void)
{
    if (log_fp) {
        fprintf(log_fp, "[PERSONALIZED QEMU] Graceful shutdown (normal exit)\n");
        fflush(log_fp);
        fclose(log_fp);
        log_fp = NULL;
    }
}

static void dummy_close_log(int signum)
{
    if (log_fp) {
        fprintf(log_fp, "[PERSONALIZED QEMU] Graceful shutdown (signal %d)\n",
                signum);
        fflush(log_fp);
        fclose(log_fp);
        log_fp = NULL;
    }
    signal(signum, SIG_DFL);
    raise(signum);
}

static void dummy_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dummy_log(const char *fmt, ...)
{
    if (!log_fp) {
        dummy_log_path = dummy_get_log_path();
        log_fp = fopen(dummy_log_path, "a");
        if (!log_fp) {
            return;
        }
        setvbuf(log_fp, NULL, _IOLBF, 0);
        if (!log_hooks_installed) {
            signal(SIGINT,  dummy_close_log);
            signal(SIGTERM, dummy_close_log);
            signal(SIGQUIT, dummy_close_log);
            signal(SIGHUP,  dummy_close_log);
            atexit(dummy_log_atexit);
            log_hooks_installed = true;
        }
        fprintf(log_fp,
                "[PERSONALIZED QEMU] Logging started (pid=%ld, file=%s)\n",
                (long)getpid(), dummy_log_path);
        fflush(log_fp);
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fflush(log_fp);
}

typedef struct ZynqMPDummyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion direct_mmio;
    uint32_t regs[NUM_REGS];
    hwaddr map_addr;
} ZynqMPDummyState;

DECLARE_INSTANCE_CHECKER(ZynqMPDummyState, ZYNQMP_DUMMY, TYPE_ZYNQMP_DUMMY)

static uint64_t dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(opaque);
    uint32_t val = s->regs[addr >> 2];
    dummy_log("[PERSONALIZED QEMU] [DUMMY PERIPH READ ] addr=0x%08X val=0x%08X\n",
              (uint32_t)(s->map_addr + addr), val);
    return val;
}

static void dummy_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(opaque);
    s->regs[addr >> 2] = (uint32_t)val;
    dummy_log("[PERSONALIZED QEMU] [DUMMY PERIPH WRITE] addr=0x%08X val=0x%08X\n",
              (uint32_t)(s->map_addr + addr), (uint32_t)val);
}

static const MemoryRegionOps dummy_ops = {
    .read  = dummy_read,
    .write = dummy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool dummy_parse_reg(FDTGenericMMap *obj, FDTGenericRegPropInfo info,
                             Error **errp)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(obj);
    s->map_addr = info.a[0];

    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    if (sbd->mmio[0].addr != (hwaddr)-1) {
        memory_region_del_subregion(get_system_memory(), sbd->mmio[0].memory);
        sbd->mmio[0].addr = (hwaddr)-1;
    }

    if (!memory_region_is_mapped(&s->direct_mmio)) {
        memory_region_init_io(&s->direct_mmio, OBJECT(obj), &dummy_ops, s,
                              "zynqmp-dummy-direct", 0x1000);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            info.a[0],
                                            &s->direct_mmio,
                                            1000);
        dummy_log("[PERSONALIZED QEMU] parse_reg: container=amba@0 addr=0x%08lX priority=0\n",
                  (unsigned long)info.a[0]);
    }
    return false;
}

static void dummy_instance_init(Object *obj)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(obj);
    s->map_addr = 0xFF3A0000;
    memory_region_init_io(&s->mmio, obj, &dummy_ops, s,
                          TYPE_ZYNQMP_DUMMY, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    dummy_log("############################################\n");
    dummy_log("#   PERSONALIZED QEMU -- Custom Build      #\n");
    dummy_log("############################################\n");
}

static void dummy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    FDTGenericMMapClass *fmc = FDT_GENERIC_MMAP_CLASS(klass);
    dc->desc = "ZynqMP Dummy Peripheral";
    fmc->parse_reg = dummy_parse_reg;
}

static const TypeInfo dummy_info = {
    .name          = TYPE_ZYNQMP_DUMMY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ZynqMPDummyState),
    .instance_init = dummy_instance_init,
    .class_init    = dummy_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { TYPE_FDT_GENERIC_MMAP },
        { }
    },
};

static void dummy_register(void)
{
    type_register_static(&dummy_info);
}
type_init(dummy_register)

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
#include "hw/misc/zynqmp_log_manager.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#define TYPE_ZYNQMP_DUMMY "zynqmp.dummy-periph"
#define NUM_REGS 4
static const char dummy_default_log_file[] = "log.txt";

typedef struct ZynqMPDummyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion direct_mmio;
    uint32_t regs[NUM_REGS];
    hwaddr map_addr;
} ZynqMPDummyState;

DECLARE_INSTANCE_CHECKER(ZynqMPDummyState, ZYNQMP_DUMMY, TYPE_ZYNQMP_DUMMY)

static uint64_t dummy_read(void *opaque, hwaddr addr, unsigned size);
static void dummy_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);

static void dummy_run_selftest(ZynqMPDummyState *s)
{
    uint32_t write_val = 0xA5A50001;
    uint32_t read_val = 0;
    char msg_box[512];

    dummy_write(s, 0, write_val, 4);
    snprintf(msg_box, sizeof(msg_box),
             "[DUMMY-SELFTEST] write @0x%08X <= 0x%08X\n",
             (uint32_t)s->map_addr,
             write_val);
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY, msg_box);

    read_val = (uint32_t)dummy_read(s, 0, 4);
    snprintf(msg_box, sizeof(msg_box),
             "[DUMMY-SELFTEST] read  @0x%08X => 0x%08X\n",
             (uint32_t)s->map_addr,
             read_val);
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY, msg_box);
}

static uint64_t dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(opaque);
    uint32_t val = s->regs[addr >> 2];
    char msg_box[512];

    snprintf(msg_box, sizeof(msg_box),
             "[PERSONALIZED QEMU] [DUMMY PERIPH READ ] addr=0x%08X val=0x%08X\n",
             (uint32_t)(s->map_addr + addr), val);
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY, msg_box);
    return val;
}

static void dummy_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(opaque);
    s->regs[addr >> 2] = (uint32_t)val;
    char msg_box[512];

    snprintf(msg_box, sizeof(msg_box),
             "[PERSONALIZED QEMU] [DUMMY PERIPH WRITE] addr=0x%08X val=0x%08X\n",
             (uint32_t)(s->map_addr + addr), (uint32_t)val);
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY, msg_box);
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
    const char *dummy_selftest;
    s->map_addr = info.a[0];

    zynqmp_log_manager_set_channel_path(ZYNQMP_LOG_CHANNEL_DUMMY,
                                        "QEMU_DUMMY_LOG_FILE",
                                        dummy_default_log_file);

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
        {
            char msg_box[512];

            snprintf(msg_box, sizeof(msg_box),
                     "[PERSONALIZED QEMU] parse_reg: container=amba@0 addr=0x%08lX priority=0\n",
                     (unsigned long)info.a[0]);
            custom_log(ZYNQMP_LOG_CHANNEL_DUMMY, msg_box);
        }

        dummy_selftest = g_getenv("QEMU_DUMMY_SELFTEST");
        if (dummy_selftest &&
            (g_strcmp0(dummy_selftest, "1") == 0 ||
             g_ascii_strcasecmp(dummy_selftest, "true") == 0 ||
             g_ascii_strcasecmp(dummy_selftest, "yes") == 0)) {
            custom_log(ZYNQMP_LOG_CHANNEL_DUMMY,
                       "[DUMMY-SELFTEST] enabled via QEMU_DUMMY_SELFTEST\n");
            dummy_run_selftest(s);
        }
    }
    return false;
}

static void dummy_instance_init(Object *obj)
{
    ZynqMPDummyState *s = ZYNQMP_DUMMY(obj);
    s->map_addr = 0xFF3A0000;
    zynqmp_log_manager_set_channel_path(ZYNQMP_LOG_CHANNEL_DUMMY,
                                        "QEMU_DUMMY_LOG_FILE",
                                        dummy_default_log_file);
    memory_region_init_io(&s->mmio, obj, &dummy_ops, s,
                          TYPE_ZYNQMP_DUMMY, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY,
                "############################################\n");
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY,
                "#   PERSONALIZED QEMU -- Custom Build      #\n");
    custom_log(ZYNQMP_LOG_CHANNEL_DUMMY,
                "############################################\n");
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

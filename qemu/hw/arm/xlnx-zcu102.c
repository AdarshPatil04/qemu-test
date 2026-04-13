#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/xlnx-zynqmp.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "sysemu/device_tree.h"
#include "qom/object.h"
#include "net/can_emu.h"
#include "audio/audio.h"

struct XlnxZCU102 {
    MachineState parent_obj;
    XlnxZynqMPState soc;
    bool secure;
    bool virt;
    CanBusState *canbus[XLNX_ZYNQMP_NUM_CAN];
    struct arm_boot_info binfo;
};

#define TYPE_ZCU102_MACHINE   MACHINE_TYPE_NAME("xlnx-zcu102")
OBJECT_DECLARE_SIMPLE_TYPE(XlnxZCU102, ZCU102_MACHINE)

static bool zcu102_get_secure(Object *obj, Error **errp) {
    return ZCU102_MACHINE(obj)->secure;
}
static void zcu102_set_secure(Object *obj, bool value, Error **errp) {
    ZCU102_MACHINE(obj)->secure = value;
}
static bool zcu102_get_virt(Object *obj, Error **errp) {
    return ZCU102_MACHINE(obj)->virt;
}
static void zcu102_set_virt(Object *obj, bool value, Error **errp) {
    ZCU102_MACHINE(obj)->virt = value;
}

static void zcu102_modify_dtb(const struct arm_boot_info *binfo, void *fdt) {
    XlnxZCU102 *s = container_of(binfo, XlnxZCU102, binfo);
    if (!s->secure) {
        char **node_path = qemu_fdt_node_path(fdt, NULL, "xlnx,zynqmp-firmware", &error_fatal);
        for (int i = 0; node_path && node_path[i]; i++) {
            qemu_fdt_setprop_string(fdt, node_path[i], "status", "disabled");
        }
        g_strfreev(node_path);
    }
}

static void xlnx_zcu102_init(MachineState *machine) {
    XlnxZCU102 *s = ZCU102_MACHINE(machine);
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_XLNX_ZYNQMP);
    object_property_set_link(OBJECT(&s->soc), "ddr-ram", OBJECT(machine->ram), &error_abort);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);
    s->binfo.ram_size = machine->ram_size;
    s->binfo.loader_start = 0;
    s->binfo.modify_dtb = zcu102_modify_dtb;
    arm_load_kernel(s->soc.boot_cpu_ptr, machine, &s->binfo);
}

static void xlnx_zcu102_machine_class_init(ObjectClass *oc, void *data) {
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->init = xlnx_zcu102_init;
    mc->ignore_memory_transaction_failures = true;
    object_class_property_add_bool(oc, "secure", zcu102_get_secure, zcu102_set_secure);
    object_class_property_add_bool(oc, "virtualization", zcu102_get_virt, zcu102_set_virt);
}

static const TypeInfo xlnx_zcu102_machine_init_typeinfo = {
    .name = TYPE_ZCU102_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = xlnx_zcu102_machine_class_init,
    .instance_size = sizeof(XlnxZCU102),
};

static void xlnx_zcu102_machine_init_register_types(void) {
    type_register_static(&xlnx_zcu102_machine_init_typeinfo);
}
type_init(xlnx_zcu102_machine_init_register_types)

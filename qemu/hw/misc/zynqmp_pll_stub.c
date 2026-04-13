/*
 * ZynqMP PLL / DDR / SERDES Bypass Peripheral for Custom QEMU
 *
 * Memory-mapped peripheral that intercepts ALL FSBL blocking registers
 * and returns "ready/locked/done" values so the FSBL completes psu_init()
 * without getting stuck in mask_poll() or raw while-loops.
 *
 * Static overlays (fixed return value):
 *   CRF_APB PLL_STATUS   0xFD1A0044  => 0x3F  (APLL/DPLL/VPLL locked+stable)
 *   CRL_APB PLL_STATUS   0xFF5E0040  => 0x1B  (IOPLL/RPLL locked+stable)
 *   DDR PHY DXnGSR0      0xFD0807E0/9E0/BE0/DE0 => 0x0001007F (DPLOCK+cal)
 *   SERDES Ln PLL_STATUS  0xFD40{2,6,A,E}3E4  => 0x11  (PLL locked)
 *   DDRC STAT             0xFD070004  => 0x01  (Normal operating mode)
 *   PMU PWRUP STATUS      0xFFD80110  => 0x00  (PL power-up complete)
 *   SERDES supply good    0xFD402B1C  => 0x0E
 *   SERDES calib done     0xFD40EF14  => 0x02
 *   SERDES PMOS code      0xFD40EF18  => 0x31
 *   SERDES NMOS code      0xFD40EF1C  => 0x31
 *   SERDES ICAL code      0xFD40EF24  => 0x0F
 *   SERDES RCAL code      0xFD40EF28  => 0x09
 *
 * Stateful overlays (DDR PHY training):
 *   DDR PHY PIR           0xFD080004  (write-intercept, transitions PGSR0)
 *   DDR PHY PGSR0         0xFD080030  (read returns phase-dependent value)
 *
 *   PGSR0 state machine (driven by PIR writes):
 *     Default:         0x8000001F  (PLL locked + basic init done)
 *     After PIR=FE01:  0x80000FFF  (all training phases complete)
 *     After PIR=0001:  0x80004FFF  (+ Vref/SRD done)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "qemu/log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ================================================================== */
/* File-based logging                                                  */
/* ================================================================== */

#define PLL_LOG_PATH_DEFAULT \
    "/home/adarshpatil04/workspace/qemu-run/qemu_backend_debug.log"

static FILE *pll_log_fp;
static bool pll_log_hooks_installed;
static const char *pll_log_path;

static const char *pll_get_log_path(void)
{
    const char *path = getenv("QEMU_PLL_LOG_FILE");

    if (!path || !path[0]) {
        path = getenv("QEMU_LOG_FILE");
    }
    if (!path || !path[0]) {
        path = PLL_LOG_PATH_DEFAULT;
    }
    return path;
}

static void pll_log_atexit(void)
{
    if (pll_log_fp) {
        fprintf(pll_log_fp, "[PLL-BYPASS] Graceful shutdown (normal exit)\n");
        fflush(pll_log_fp);
        fclose(pll_log_fp);
        pll_log_fp = NULL;
    }
}

static void pll_log_close(int signum)
{
    if (pll_log_fp) {
        fprintf(pll_log_fp,
                "[PLL-BYPASS] Graceful shutdown (signal %d)\n", signum);
        fflush(pll_log_fp);
        fclose(pll_log_fp);
        pll_log_fp = NULL;
    }
    signal(signum, SIG_DFL);
    raise(signum);
}

static void pll_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void pll_log(const char *fmt, ...)
{
    if (!pll_log_fp) {
        pll_log_path = pll_get_log_path();
        pll_log_fp = fopen(pll_log_path, "a");
        if (!pll_log_fp) {
            return;
        }
        setvbuf(pll_log_fp, NULL, _IOLBF, 0);
        if (!pll_log_hooks_installed) {
            signal(SIGINT,  pll_log_close);
            signal(SIGTERM, pll_log_close);
            signal(SIGQUIT, pll_log_close);
            signal(SIGHUP,  pll_log_close);
            atexit(pll_log_atexit);
            pll_log_hooks_installed = true;
        }
        fprintf(pll_log_fp,
                "[PLL-BYPASS] Logging started (pid=%ld, file=%s)\n",
                (long)getpid(), pll_log_path);
        fflush(pll_log_fp);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(pll_log_fp, fmt, ap);
    va_end(ap);
    fflush(pll_log_fp);
}

/* ================================================================== */
/* Static overlays -- simple registers that return a fixed value       */
/* ================================================================== */

typedef struct {
    hwaddr      addr;
    uint32_t    size;
    uint32_t    value;
    const char *name;
} StaticOverlay;

static const StaticOverlay static_overlays[] = {

    /* DDR clock (IOPLL + RPLL only) */
    { 0xFF5E0040, 4, 0x00000003, "CRL_PLL_STATUS" },

    /* DDR PHY */
    { 0xFD0807E0, 4, 0x0001007F, "DDR_DX0GSR0" },
    { 0xFD0809E0, 4, 0x0001007F, "DDR_DX2GSR0" },
    { 0xFD080BE0, 4, 0x0001007F, "DDR_DX4GSR0" },
    { 0xFD080DE0, 4, 0x0001007F, "DDR_DX6GSR0" },

    /* DDR controller */
    { 0xFD070004, 4, 0x00000001, "DDRC_STAT" },
};

#define NUM_STATIC_OVERLAYS \
    (sizeof(static_overlays) / sizeof(static_overlays[0]))

typedef struct {
    MemoryRegion mr;
    uint32_t     value;
    const char  *name;
    hwaddr       addr;
} StaticOverlayCtx;

static StaticOverlayCtx static_ctx[NUM_STATIC_OVERLAYS];
static MemoryRegion     static_mrs[NUM_STATIC_OVERLAYS];

static uint64_t static_overlay_read(void *opaque, hwaddr offset, unsigned size)
{
    StaticOverlayCtx *ctx = opaque;
    pll_log("[PLL-BYPASS] READ  %-22s @ 0x%08lX => 0x%08X\n",
            ctx->name, (unsigned long)(ctx->addr + offset), ctx->value);
    return ctx->value;
}

static void static_overlay_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    StaticOverlayCtx *ctx = opaque;
    pll_log("[PLL-BYPASS] WRITE %-22s @ 0x%08lX <= 0x%08lX (ignored)\n",
            ctx->name, (unsigned long)(ctx->addr + offset),
            (unsigned long)value);
}

static const MemoryRegionOps static_overlay_ops = {
    .read  = static_overlay_read,
    .write = static_overlay_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ================================================================== */
/* Stateful DDR PHY overlay -- PGSR0 transitions based on PIR writes  */
/*                                                                     */
/* psu_ddr_phybringup_data() sequence:                                 */
/*   1. PIR=0x00040010/11  -> poll PGSR0 bit 0          (IDONE)       */
/*   2. Check PGSR0 bit 31 + DXnGSR0 bit 16             (PLL locks)  */
/*   3. PIR=0x00040063     -> poll PGSR0 bits[3:0]=0xF   (cal done)   */
/*   4. prog_reg(PIR,1)   -> poll PGSR0 bits[7:0]=0x1F  (init done)  */
/*   5. PIR=0x0004FE01    -> poll PGSR0==0x80000FFF      (training)   */
/*   6. PIR=0x00060001    -> poll PGSR0 & 0x80004001     (Vref)       */
/*   7. PIR=0x0000C001    -> poll PGSR0 & 0x80000C01     (SRD)        */
/*                                                                     */
/* Our state machine:                                                  */
/*   default          -> PGSR0 = 0x8000001F   (satisfies 1-4)         */
/*   PIR bit 9 set    -> PGSR0 = 0x80000FFF   (satisfies 5)           */
/*   PIR bit 17 set   -> PGSR0 = 0x80004FFF   (satisfies 6-7)         */
/* ================================================================== */

static uint32_t pgsr0_current = 0x8000001F;
static uint32_t pir_stored    = 0;

static MemoryRegion pgsr0_mr;
static MemoryRegion pir_mr;

static uint64_t pgsr0_read(void *opaque, hwaddr offset, unsigned size)
{
    pll_log("[PLL-BYPASS] READ  DDR_PGSR0              @ 0x%08lX => 0x%08X\n",
            (unsigned long)(0xFD080030 + offset), pgsr0_current);
    return pgsr0_current;
}

static void pgsr0_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    pll_log("[PLL-BYPASS] WRITE DDR_PGSR0              @ 0x%08lX <= 0x%08lX "
            "(ignored)\n",
            (unsigned long)(0xFD080030 + offset), (unsigned long)value);
}

static const MemoryRegionOps pgsr0_ops = {
    .read  = pgsr0_read,
    .write = pgsr0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t pir_read(void *opaque, hwaddr offset, unsigned size)
{
    pll_log("[PLL-BYPASS] READ  DDR_PIR                @ 0x%08lX => 0x%08X\n",
            (unsigned long)(0xFD080004 + offset), pir_stored);
    return pir_stored;
}

static void pir_write(void *opaque, hwaddr offset,
                       uint64_t value, unsigned size)
{
    uint32_t val = (uint32_t)value;
    uint32_t old_pgsr0 = pgsr0_current;

    pir_stored = val;

    /*
     * Transition PGSR0 based on the training command written to PIR.
     *
     * PIR = 0x0004FE01: full DDR training (WL, DQS gate, WLA, RDDSKW,
     *                   WRDSKW, RdEye, WrEye) -> all training done
     * PIR = 0x00060001: Vref training          -> Vref done
     * PIR = 0x0000C001: static read/DATX8 cal  -> SRD done
     *
     * We detect training phases by checking unique PIR bits:
     *   bit  9 (0x200) set in 0x0004FE01 -> full training
     *   bit 17 (0x20000) set in 0x00060001 -> Vref training
     *
     * For 0x0000C001 (bits 15:14 set), PGSR0 stays at 0x80004FFF
     * which already satisfies (PGSR0 & 0x80000C01) == 0x80000C01.
     */
    if (val & 0x00000200) {
        /* Full training: PGSR0 = all training done flags + APLOCK */
        pgsr0_current = 0x80000FFF;
    } else if (val & 0x00020000) {
        /* Vref training: add VDONE (bit 14) */
        pgsr0_current = 0x80004FFF;
    } else if ((val & 0x0000C000) == 0x0000C000) {
        /* SRD + DATX8: keep full value (bits 10,11 already set in 0xFFF) */
        pgsr0_current = 0x80004FFF;
    }
    /* All other PIR writes (PLL init, basic cal) keep current PGSR0 */

    pll_log("[PLL-BYPASS] WRITE DDR_PIR                @ 0x%08lX <= 0x%08X  "
            "PGSR0: 0x%08X -> 0x%08X\n",
            (unsigned long)(0xFD080004 + offset), val,
            old_pgsr0, pgsr0_current);
}

static const MemoryRegionOps pir_ops = {
    .read  = pir_read,
    .write = pir_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ================================================================== */
/* Machine-init notifier -- maps all overlays onto system memory       */
/* ================================================================== */

static void pll_bypass_machine_init_notify(Notifier *notifier, void *data)
{
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    pll_log("############################################\n");
    pll_log("#   PERSONALIZED QEMU -- PLL Bypass v2     #\n");
    pll_log("#   Static overlays: %u                     #\n",
            (unsigned)NUM_STATIC_OVERLAYS);
    pll_log("#   Stateful overlays: PIR + PGSR0         #\n");
    pll_log("############################################\n");

    /* Map static overlays */
    for (i = 0; i < NUM_STATIC_OVERLAYS; i++) {
        const StaticOverlay *ov = &static_overlays[i];
        StaticOverlayCtx *ctx  = &static_ctx[i];

        ctx->value = ov->value;
        ctx->name  = ov->name;
        ctx->addr  = ov->addr;

        memory_region_init_io(&static_mrs[i], NULL, &static_overlay_ops,
                              ctx, ov->name, ov->size);
        memory_region_add_subregion_overlap(sysmem, ov->addr,
                                            &static_mrs[i], 10000);

        pll_log("[PLL-BYPASS] Mapped %-22s @ 0x%08lX  val=0x%08X\n",
                ov->name, (unsigned long)ov->addr, ov->value);
    }

    /* Map stateful DDR PHY overlays */
    memory_region_init_io(&pir_mr, NULL, &pir_ops, NULL,
                          "DDR_PIR", 4);
    memory_region_add_subregion_overlap(sysmem, 0xFD080004,
                                        &pir_mr, 10000);
    pll_log("[PLL-BYPASS] Mapped DDR_PIR                @ 0xFD080004  "
            "(stateful)\n");

    memory_region_init_io(&pgsr0_mr, NULL, &pgsr0_ops, NULL,
                          "DDR_PGSR0", 4);
    memory_region_add_subregion_overlap(sysmem, 0xFD080030,
                                        &pgsr0_mr, 10000);
    pll_log("[PLL-BYPASS] Mapped DDR_PGSR0              @ 0xFD080030  "
            "val=0x%08X (stateful)\n", pgsr0_current);

    pll_log("[PLL-BYPASS] Total: %u static + 2 stateful overlays active\n",
            (unsigned)NUM_STATIC_OVERLAYS);
}

static Notifier pll_bypass_notifier = {
    .notify = pll_bypass_machine_init_notify,
};

static void pll_bypass_register(void)
{
    qemu_add_machine_init_done_notifier(&pll_bypass_notifier);
}

type_init(pll_bypass_register)

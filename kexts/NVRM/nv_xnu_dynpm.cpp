/*
 * G15 fork (g15-gc6), 17/11/2026: the OS side of NVIDIA's dynamic power management (RTD3/GC6), step 3a.
 * Opt-in with the boot argument nvrmdynpm=1 (and the RM registry key DynamicPowerManagement=2 through nvregdwords).
 * nv_dynamic_power_available answers yes; the idle / not-idle / runtime-suspend callbacks only log and count for now, so
 * RM can arm its dynamic power state machine and we can see what it asks for. Nothing is suspended here yet.
 */
#include "nv-xnu.h"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

static int nv_xnu_dynpm_cache = -1;
extern "C" bool nv_xnu_dynpm_on(void)
{
    if (nv_xnu_dynpm_cache < 0) { int v = 0; nv_xnu_dynpm_cache = (PE_parse_boot_argn("nvrmdynpm", &v, sizeof v) && v == 1) ? 1 : 0; }
    return nv_xnu_dynpm_cache == 1;
}
static volatile SInt32 nIdle, nNotIdle, nAllow, nDisallow, nHoldoff, nMmap;
#define DYNPM_LOG(cnt, what) do { SInt32 c = OSIncrementAtomic(&cnt) + 1; if (c <= 20 || (c % 100) == 0) \
    { kprintf("NVRM-xnu: dynpm: %s (#%d)\n", what, (int)c); IOLog("NVRM-xnu: dynpm: %s (#%d)\n", what, (int)c); } } while (0)

NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *) { return nv_xnu_dynpm_on() ? NV_TRUE : NV_FALSE; }
void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nAllow, "RM allows runtime suspend"); }
void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nDisallow, "RM disallows runtime suspend"); }
NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *) { DYNPM_LOG(nIdle, "RM indicates IDLE"); return NV_OK; }
NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *) { DYNPM_LOG(nNotIdle, "RM indicates NOT idle"); return NV_OK; }
void NV_API_CALL nv_idle_holdoff(nv_state_t *) { DYNPM_LOG(nHoldoff, "RM idle holdoff"); }
void NV_API_CALL nv_set_safe_to_mmap_locked(nv_state_t *, NvBool safe) { DYNPM_LOG(nMmap, safe ? "safe to mmap: yes" : "safe to mmap: NO"); }

// Snapshot of RM's dynamic power bookkeeping (patch 620-g15-dynpm-debug.patch in dynamic-power.c), published on the
// RTX PCI entry as nvrm-dynpm-state: what keeps the refcount above 0 so RM never indicates idle. Callers are offsets
// from rm_xnu_dynamic_power_snapshot; nm NVRM gives that symbol's address, add the offset and atos/nm resolves them.
extern "C" void NV_API_CALL rm_xnu_dynamic_power_snapshot(nv_state_t *nv, NvU32 *pOut, NvU64 *pCallers);
extern "C" void nv_xnu_dynpm_publish(nv_state_t *nv, IORegistryEntry *where)
{
    if (!nv_xnu_dynpm_on() || !nv || !where) return;
    NvU32 v[22] = {}; NvU64 c[8] = {};
    rm_xnu_dynamic_power_snapshot(nv, v, c);
    static const char *const st[] = { "unknown", "in-use", "idle-instant", "idle-sustained", "idle-indicated" };
    static const char *const md[] = { "never", "coarse", "fine" };
    char b[800]; int n;
    n = snprintf(b, sizeof b, "mode %s | state %s | refcount %d | deferred %u | idlecheck %u | holdoff %u | sustainedq %u | "
                 "gcoffDisallow %u | console %u | primaryVga %u | ref c/f %u/%u unref c/f %u/%u | cb idle %d notidle %d allow %d disallow %d | "
                 "gcx runs %u fail %u gc6ok %u gcoffok %u neither %u | upstream %s | fbMB %u gate %s | callers",
                 v[0] < 3 ? md[v[0]] : "?", v[1] < 5 ? st[v[1]] : "?", (int)v[2], v[3], v[4], v[5], v[6], v[7],
                 (unsigned)nv->console_device, (unsigned)nv->primary_vga, v[9], v[10], v[12], v[13],
                 (int)nIdle, (int)nNotIdle, (int)nAllow, (int)nDisallow,
                 v[14], v[15], v[16], v[17], v[18], v[20] == 1 ? "yes" : v[20] == 2 ? "no" : "-", v[19],
                 v[21] == 1 ? "pass" : v[21] == 2 ? "fail" : "-");
    const uintptr_t anchor = (uintptr_t)&rm_xnu_dynamic_power_snapshot;
    for (int i = 0; i < 8 && n > 0 && n < (int)sizeof b - 32; i++) {
        if (!c[i]) continue;
        const uintptr_t a = (uintptr_t)(c[i] & 0x3fffffffffffffffull);
        n += snprintf(b + n, sizeof b - (size_t)n, " %c%+lld", (c[i] >> 62) == 2 ? 'f' : 'c', (long long)((intptr_t)a - (intptr_t)anchor));
    }
    where->setProperty("nvrm-dynpm-state", b);
}

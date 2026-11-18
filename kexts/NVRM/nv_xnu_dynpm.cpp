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

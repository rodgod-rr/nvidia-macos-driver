/*
 * G15 fork (g15-gc6), 17/11/2026: the OS side of NVIDIA's dynamic power management (RTD3/GC6), step 3a.
 * Opt-in with the boot argument nvrmdynpm=1 (and the RM registry key DynamicPowerManagement=2 through nvregdwords).
 * nv_dynamic_power_available answers yes; the idle / not-idle / runtime-suspend callbacks only log and count for now, so
 * RM can arm its dynamic power state machine and we can see what it asks for. Nothing is suspended here yet.
 * Step E2 (21/11/2026): real revocation of user CPU mappings (nv_revoke_gpu_mappings & co., end of this file).
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

static IOLock *rvLock(void);
NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *) { if (nv_xnu_dynpm_on()) rvLock(); return nv_xnu_dynpm_on() ? NV_TRUE : NV_FALSE; }
void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nAllow, "RM allows runtime suspend"); }
void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nDisallow, "RM disallows runtime suspend"); }
// G15: RM's idle decision is published on the RTX PCI entry as NVRMIdle (Yes/No) and NVRMIdleCount, from a thread
// call (the callers hold RM's dynamic power mutex, so no registry work in their context). Nothing is powered down here:
// G15.app parks the RTX through G15Fan RtxPm (revoke + suspend + GOFF) after NVRMIdle stays Yes long enough, and the
// G15PM wake hooks bring it back before a client reaches RM. A real GC6 runtime suspend is the next step (docs/RTX-GC6.md).
#include <kern/thread_call.h>
static volatile SInt32 gRmIdle;
static IOPCIDevice *gIdlePci;
static thread_call_t gIdleCall;
static void nv_xnu_rv_publish(IORegistryEntry *where);
static void nv_xnu_idle_publish(thread_call_param_t, thread_call_param_t)
{
    if (!gIdlePci) return;
    gIdlePci->setProperty("NVRMIdle", gRmIdle ? kOSBooleanTrue : kOSBooleanFalse);
    gIdlePci->setProperty("NVRMIdleCount", (unsigned long long)nIdle, 32);
    nv_xnu_rv_publish(gIdlePci);
}
static void nv_xnu_idle_note(nv_state_t *nv, bool idle)
{
    gRmIdle = idle ? 1 : 0;
    if (!gIdlePci && nv && nv->handle) gIdlePci = ((nv_xnu_pci_slot *)nv->handle)->pci;
    if (!gIdleCall) gIdleCall = thread_call_allocate(nv_xnu_idle_publish, nullptr);
    if (gIdleCall) thread_call_enter(gIdleCall);
}
NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *nv) { DYNPM_LOG(nIdle, "RM indicates IDLE"); nv_xnu_idle_note(nv, true); return NV_OK; }
NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *nv) { DYNPM_LOG(nNotIdle, "RM indicates NOT idle"); nv_xnu_idle_note(nv, false); return NV_OK; }
void NV_API_CALL nv_idle_holdoff(nv_state_t *) { DYNPM_LOG(nHoldoff, "RM idle holdoff"); }

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

// True while RM counts the GPU as idle (idle-instant / sustained / indicated). The utilization sampler asks RM once a
// second; each RM API call takes a dynamic power reference and puts the state back to in-use, so RM never saw two idle
// checks (5 s each) in a row and never indicated idle (like nvidia-smi polling on Linux). An idle GPU is at 0 %.
extern "C" bool nv_xnu_dynpm_gpu_idle(nv_state_t *nv)
{
    if (!nv_xnu_dynpm_on() || !nv) return false;
    NvU32 v[22] = {};
    rm_xnu_dynamic_power_snapshot(nv, v, NULL);
    return v[0] == 2 /* fine */ && v[1] >= 2 /* idle-instant, idle-sustained, idle-indicated */;
}

// ---- Revocation of user CPU mappings of the GPU (step E2, 21/11/2026; docs/RTX-GC6.md) ----------------------------
// Linux (nv-mmap.c): before indicating idle RM calls nv_revoke_gpu_mappings (unmap_mapping_range on every open file);
// nv_get_all_mappings_revoked_locked says whether nothing re-mapped since; RmIndicateIdle clears safe_to_mmap, and a
// fault while it is clear schedules rm_schedule_gpu_wakeup (RmForceGpuNotIdle: indicate not idle, set safe again) and
// the faulting thread retries until the GPU is back. A fault while safe re-inserts the pages and clears
// all_mappings_revoked, so RM goes back to idle-instant. All of it under nvl->mmap_lock (nv_acquire_mmap_lock).
//
// macOS: user mappings of the GPU are the physical descriptors NVRMUserClient::clientMemoryForType hands out (BAR0
// registers for CoreDisplay, BAR1/VRAM and USERMODE doorbells for NVK); clientMemoryForType registers each one here.
// Revoking = IOMemoryDescriptor::redirect(NULL, true): IOKit drops the pages from every task (IOUnmapPages) and sets
// kIOMemoryRedirected. A single-range physical descriptor has a CONTIGUOUS device pager (dp.pagerContig): its vm object
// has no vm pages, so every fault calls memory_object_data_request -> device_data_action ->
// IOMemoryDescriptor::handleFault, which sleeps on the descriptor's address (IORecursiveLockSleep(gIOMemoryLock, this))
// while kIOMemoryRedirected is set; redirect(NULL, false) clears it and wakes the sleepers, and the fault completes
// against the same physical pages - the CPU access waits, nothing is lost. A multi-range descriptor has a
// non-contiguous pager whose private pages stay in the object after populateDevicePager, so a re-fault maps them again
// without asking the pager: it cannot be blocked. While one of those is alive, nothing is revoked and RM is told "not
// all revoked" (it then never indicates idle).
// No hook on handleFault (the G15PM prototype kern_revoke.cpp had one): while descriptors are redirected a thread call
// polls them with thread_wakeup_prim(md, one thread), which returns KERN_SUCCESS only when a thread was sleeping on that
// event - a fault. The woken thread re-checks kIOMemoryRedirected and sleeps again (handleFault loops), so the probe is
// harmless. A fault while safe restores everything at once (like Linux's re-insert); a fault while not safe asks RM
// for the GPU (rm_schedule_gpu_wakeup, once per idle period, as Linux's gpu_wakeup_callback_needed), and RM's
// nv_set_safe_to_mmap_locked(TRUE) restores everything (thread call: no IOKit work inside RM's locks).
// Watchdog: a fault not served in kRvWatchdogMs is restored anyway (nothing is powered off before step E3, so the
// pages are valid; logged and counted). Gate: nvrmdynpm=1. With 0 nothing here runs and clientMemoryForType is as before.
// Lock order: RM dynamic power mutex -> gMmapLock -> gIOMemoryLock. Nothing here calls RM with gMmapLock held.
// Counters: nvrm-revoke on the RTX PCI entry.
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>

namespace {
struct RvEntry { IOMemoryDescriptor *md; bool redirected; bool multi; };
const unsigned kRvMax = 4096;
const uint32_t kRvPollMs = 15, kRvWatchdogMs = 2000;
RvEntry gRv[kRvMax];
unsigned gRvN;               // tracked descriptors (each retained once)
unsigned gRvRedirected;      // tracked and redirected now
unsigned gRvMulti;           // tracked multi-range (cannot be blocked)
unsigned gRvOverflow;        // descriptors the table had no room for: nothing counts as revoked again this boot
bool gRvSafe = true;         // RM's safe_to_mmap
bool gRvAllRevoked = true;   // Linux all_mappings_revoked (TRUE before any mapping, as nv-pci.c starts it)
bool gRvWakeNeeded = true;   // Linux gpu_wakeup_callback_needed
uint64_t gRvFaultSince;      // mach time of the first fault still waiting for RM (0 = none)
nv_state_t *gRvNv;
IOLock *gMmapLock;
thread_call_t gRvPollCall, gRvRestoreCall;
volatile SInt32 gRvPollArmed;
struct { uint32_t revokes, redirects, restores, faultsSafe, faultsWake, wakeReqs, wakeFail, watchdog, newBlocked, refused; uint64_t lastFaultPhys; } gRs;
}

static IOLock *rvLock(void)
{
    if (!gMmapLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gMmapLock)) IOLockFree(l);
    }
    return gMmapLock;
}
// RM's nvl->mmap_lock. RM takes it in sleepable contexts only (work items, its timer thread call).
void NV_API_CALL nv_acquire_mmap_lock(nv_state_t *) { if (nv_xnu_dynpm_on() && rvLock()) IOLockLock(gMmapLock); }
void NV_API_CALL nv_release_mmap_lock(nv_state_t *) { if (nv_xnu_dynpm_on() && gMmapLock) IOLockUnlock(gMmapLock); }

static void rvPoll(thread_call_param_t, thread_call_param_t);
static void rvRestoreFn(thread_call_param_t, thread_call_param_t);
static void rvPublishSoon(void)
{
    if (!gIdleCall) gIdleCall = thread_call_allocate(nv_xnu_idle_publish, nullptr);
    if (gIdleCall && gIdlePci) thread_call_enter(gIdleCall);
}
static void rvInit(nv_state_t *nv)
{
    if (nv && !gRvNv) gRvNv = nv;
    if (!gIdlePci && nv && nv->handle) gIdlePci = ((nv_xnu_pci_slot *)nv->handle)->pci;
    if (!gRvPollCall) { thread_call_t c = thread_call_allocate(rvPoll, nullptr); if (c && !OSCompareAndSwapPtr(nullptr, c, (void * volatile *)&gRvPollCall)) thread_call_free(c); }
    if (!gRvRestoreCall) { thread_call_t c = thread_call_allocate(rvRestoreFn, nullptr); if (c && !OSCompareAndSwapPtr(nullptr, c, (void * volatile *)&gRvRestoreCall)) thread_call_free(c); }
}

// Everything below with "Locked" runs under gMmapLock.
static void rvPruneLocked(void)
{
    unsigned w = 0;
    for (unsigned i = 0; i < gRvN; i++) {
        RvEntry e = gRv[i];
        if (e.md->getRetainCount() <= 1) {             // only we hold it: its mappings are gone
            if (e.redirected) gRvRedirected--;
            if (e.multi) gRvMulti--;
            e.md->release();
            continue;
        }
        gRv[w++] = e;
    }
    gRvN = w;
}
static void rvRedirectLocked(RvEntry &e, bool on)
{
    if (e.multi || e.redirected == on) return;
    e.md->redirect(nullptr, on);
    e.redirected = on;
    if (on) { gRvRedirected++; gRs.redirects++; } else gRvRedirected--;
}
static void rvRestoreAllLocked(void)
{
    if (!gRvRedirected) return;
    for (unsigned i = 0; i < gRvN; i++) rvRedirectLocked(gRv[i], false);
    gRvAllRevoked = false;
    gRvFaultSince = 0;
    gRs.restores++;
}
static void rvPollArmLocked(void)
{
    if (!gRvRedirected || !gRvPollCall || !OSCompareAndSwap(0, 1, &gRvPollArmed)) return;
    uint64_t dl; clock_interval_to_deadline(kRvPollMs, kMillisecondScale, &dl);
    thread_call_enter_delayed(gRvPollCall, dl);
}
static void rvWakeGpu(nv_state_t *nv)
{
    if (!nv) return;
    NV_STATUS st = rm_schedule_gpu_wakeup(NULL, nv);
    IOLockLock(gMmapLock);
    gRs.wakeReqs++;
    if (st != NV_OK) { gRs.wakeFail++; gRvWakeNeeded = true; }
    IOLockUnlock(gMmapLock);
    if (gRs.wakeReqs <= 20 || st != NV_OK)
        IOLog("NVRM-xnu: dynpm: revoked mapping touched, GPU wakeup requested (#%u, status 0x%x)\n", gRs.wakeReqs, (unsigned)st);
}

static void rvPoll(thread_call_param_t, thread_call_param_t)
{
    bool wake = false, faulted = false, dog = false;
    nv_state_t *nv = nullptr;
    IOLockLock(gMmapLock);
    gRvPollArmed = 0;
    for (unsigned i = 0; i < gRvN; i++) {
        if (!gRv[i].redirected) continue;
        if (thread_wakeup_prim((event_t)gRv[i].md, TRUE, THREAD_AWAKENED) == KERN_SUCCESS) {
            faulted = true;
            IOByteCount len = 0;
            gRs.lastFaultPhys = gRv[i].md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
        }
    }
    if (faulted && gRvSafe) { gRs.faultsSafe++; rvRestoreAllLocked(); }
    else if (faulted) {
        if (!gRvFaultSince) { gRvFaultSince = mach_absolute_time(); gRs.faultsWake++; }
        if (gRvWakeNeeded) { gRvWakeNeeded = false; wake = true; nv = gRvNv; }
    }
    if (gRvFaultSince && gRvRedirected) {
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - gRvFaultSince, &ns);
        if (ns > (uint64_t)kRvWatchdogMs * 1000000ull) { gRs.watchdog++; rvRestoreAllLocked(); dog = true; }
    }
    rvPollArmLocked();
    IOLockUnlock(gMmapLock);
    if (dog) IOLog("NVRM-xnu: dynpm: revoke WATCHDOG: a fault waited %u ms for RM, mappings restored (#%u)\n", kRvWatchdogMs, gRs.watchdog);
    if (wake) rvWakeGpu(nv);
    if (faulted || dog) rvPublishSoon();
}
static void rvRestoreFn(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gMmapLock);
    if (gRvSafe) rvRestoreAllLocked();
    IOLockUnlock(gMmapLock);
    rvPublishSoon();
}

// NVRMUserClient::clientMemoryForType: a physical descriptor of card memory about to be mapped into a task.
extern "C" void nv_xnu_rv_track(IOMemoryDescriptor *md, bool multi)
{
    if (!nv_xnu_dynpm_on() || !md || !rvLock()) return;
    rvInit(nullptr);
    bool wake = false; nv_state_t *nv = nullptr;
    IOLockLock(gMmapLock);
    rvPruneLocked();
    for (unsigned i = 0; i < gRvN; i++) if (gRv[i].md == md) { IOLockUnlock(gMmapLock); return; }
    if (gRvN >= kRvMax) {
        gRvOverflow++; gRvAllRevoked = false;
        IOLockUnlock(gMmapLock);
        if (gRvOverflow == 1) IOLog("NVRM-xnu: dynpm: revoke table full (%u): RM will not be told mappings are revoked\n", kRvMax);
        return;
    }
    md->retain();
    RvEntry &e = gRv[gRvN++];
    e.md = md; e.redirected = false; e.multi = multi;
    if (multi) gRvMulti++;
    if (!gRvSafe) {
        // RM indicated idle: the new mapping's first access must bring the GPU back (Linux maps no pages then)
        if (!multi) { rvRedirectLocked(e, true); gRs.newBlocked++; rvPollArmLocked(); }
        else if (gRvWakeNeeded) { gRvWakeNeeded = false; wake = true; nv = gRvNv; }
    } else {
        gRvAllRevoked = false;                                         // as Linux's mmap while safe
        if (gRvRedirected && gRvRestoreCall) thread_call_enter(gRvRestoreCall);
    }
    IOLockUnlock(gMmapLock);
    if (wake) rvWakeGpu(nv);
    rvPublishSoon();
}

static volatile SInt32 nRevoke;
NV_STATUS NV_API_CALL nv_revoke_gpu_mappings(nv_state_t *nv)
{
    DYNPM_LOG(nRevoke, "RM revokes GPU mappings");
    if (!nv_xnu_dynpm_on() || !rvLock()) return NV_OK;
    rvInit(nv);
    IOLockLock(gMmapLock);
    gRs.revokes++;
    rvPruneLocked();
    if (gRvMulti || gRvOverflow) { gRs.refused++; gRvAllRevoked = false; }
    else {
        for (unsigned i = 0; i < gRvN; i++) rvRedirectLocked(gRv[i], true);
        gRvAllRevoked = true;
        rvPollArmLocked();
    }
    IOLockUnlock(gMmapLock);
    rvPublishSoon();
    return NV_OK;
}
// Caller holds nv_acquire_mmap_lock. Yes only when every live user mapping of the card is redirected and none touched.
NvBool NV_API_CALL nv_get_all_mappings_revoked_locked(nv_state_t *)
{
    if (!nv_xnu_dynpm_on()) return NV_FALSE;
    return (gRvAllRevoked && !gRvMulti && !gRvOverflow && gRvRedirected == gRvN) ? NV_TRUE : NV_FALSE;
}
// Caller holds nv_acquire_mmap_lock. FALSE: RM indicated idle (new mappings start redirected, a fault wakes the GPU).
// TRUE: the GPU is guaranteed powered; restore what is redirected.
void NV_API_CALL nv_set_safe_to_mmap_locked(nv_state_t *, NvBool safe)
{
    DYNPM_LOG(nMmap, safe ? "safe to mmap: yes" : "safe to mmap: NO");
    if (!nv_xnu_dynpm_on()) return;
    if (!safe && gRvSafe) gRvWakeNeeded = true;
    gRvSafe = safe ? true : false;
    if (safe && gRvRedirected && gRvRestoreCall) thread_call_enter(gRvRestoreCall);
}

static void nv_xnu_rv_publish(IORegistryEntry *where)
{
    if (!nv_xnu_dynpm_on() || !where) return;
    char b[480];
    snprintf(b, sizeof b, "tracked %u | redirected %u | multi %u | overflow %u | safe %s | allRevoked %s | revokes %u refused %u "
             "redirects %u restores %u | faults safe %u wake %u | wakeReq %u fail %u | watchdog %u | newBlocked %u | lastFault 0x%llx",
             gRvN, gRvRedirected, gRvMulti, gRvOverflow, gRvSafe ? "yes" : "NO", gRvAllRevoked ? "yes" : "no", gRs.revokes,
             gRs.refused, gRs.redirects, gRs.restores, gRs.faultsSafe, gRs.faultsWake, gRs.wakeReqs, gRs.wakeFail,
             gRs.watchdog, gRs.newBlocked, (unsigned long long)gRs.lastFaultPhys);
    where->setProperty("nvrm-revoke", b);
}

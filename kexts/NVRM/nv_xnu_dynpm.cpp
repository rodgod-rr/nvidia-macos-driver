/*
 * G15 fork (g15-gc6), 17/11/2026: the OS side of NVIDIA's dynamic power management (RTD3/GC6), step 3a.
 * Opt-in with the boot argument nvrmdynpm=1 (and the RM registry key DynamicPowerManagement=2 through nvregdwords).
 * nv_dynamic_power_available answers yes; the idle / not-idle / runtime-suspend callbacks only log and count for now, so
 * RM can arm its dynamic power state machine and we can see what it asks for. Nothing is suspended here yet.
 * Step E2 (21/11/2026): real revocation of user CPU mappings (nv_revoke_gpu_mappings & co., end of this file).
 * Step E3 (22/11/2026): nvrmdynpm=2 executes RM's decision: runtime suspend into GC6 and a synchronous way back.
 */
#include "nv-xnu.h"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>

// nvrmdynpm=1: RM's dynamic power runs, revokes and decides idle, nothing is powered down (E1/E2).
// nvrmdynpm=2: RM also executes the runtime suspend (GC6) from nv_indicate_idle, and the way back is synchronous (E3).
static int nv_xnu_dynpm_cache = -1;
static int nv_xnu_dynpm_mode(void)
{
    if (nv_xnu_dynpm_cache < 0) {
        int v = 0;
        nv_xnu_dynpm_cache = (PE_parse_boot_argn("nvrmdynpm", &v, sizeof v) && (v == 1 || v == 2)) ? v : 0;
    }
    return nv_xnu_dynpm_cache;
}
extern "C" bool nv_xnu_dynpm_on(void) { return nv_xnu_dynpm_mode() >= 1; }
extern "C" bool nv_xnu_dynpm_exec(void) { return nv_xnu_dynpm_mode() == 2; }
static volatile SInt32 nIdle, nNotIdle, nAllow, nDisallow, nHoldoff, nMmap;
#define DYNPM_LOG(cnt, what) do { SInt32 c = OSIncrementAtomic(&cnt) + 1; if (c <= 20 || (c % 100) == 0) \
    { kprintf("NVRM-xnu: dynpm: %s (#%d)\n", what, (int)c); IOLog("NVRM-xnu: dynpm: %s (#%d)\n", what, (int)c); } } while (0)

static IOLock *rvLock(void);
static void pmInit(nv_state_t *nv);
static void pmGet(void);
static void pmPut(void);
static NV_STATUS pmResumeSync(const char *why);
NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *nv)
{
    if (!nv_xnu_dynpm_on()) return NV_FALSE;
    rvLock();
    pmInit(nv);
    return NV_TRUE;
}
void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nAllow, "RM allows runtime suspend"); pmPut(); }
void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *) { DYNPM_LOG(nDisallow, "RM disallows runtime suspend"); pmGet(); }
// G15: RM's idle decision is published on the RTX PCI entry as NVRMIdle (Yes/No) and NVRMIdleCount, from a thread
// call (the callers hold RM's dynamic power mutex, so no registry work in their context). With nvrmdynpm=1 nothing is
// powered down here. With nvrmdynpm=2 (step E3) the runtime suspend below executes RM's decision (GC6).
static volatile SInt32 gRmIdle;
static IOPCIDevice *gIdlePci;
static thread_call_t gIdleCall;
static void nv_xnu_rv_publish(IORegistryEntry *where);
static void nv_xnu_rtd3_publish(IORegistryEntry *where);
static void nv_xnu_idle_publish(thread_call_param_t, thread_call_param_t)
{
    if (!gIdlePci) return;
    gIdlePci->setProperty("NVRMIdle", gRmIdle ? kOSBooleanTrue : kOSBooleanFalse);
    gIdlePci->setProperty("NVRMIdleCount", (unsigned long long)nIdle, 32);
    nv_xnu_rv_publish(gIdlePci);
    nv_xnu_rtd3_publish(gIdlePci);
}
static void nv_xnu_idle_note(nv_state_t *nv, bool idle)
{
    gRmIdle = idle ? 1 : 0;
    if (!gIdlePci && nv && nv->handle) gIdlePci = ((nv_xnu_pci_slot *)nv->handle)->pci;
    if (!gIdleCall) gIdleCall = thread_call_allocate(nv_xnu_idle_publish, nullptr);
    if (gIdleCall) thread_call_enter(gIdleCall);
}
// Linux: nv_indicate_idle = pm_runtime_put_noidle + a sysfs read that runs the PM core's idle check (autosuspend 500 ms
// later: nv_pmops_runtime_suspend -> rm_transition_dynamic_power(enter)). nv_indicate_not_idle = pm_runtime_get_noresume
// + pci_bus_type.shutdown, whose pci_device_shutdown does pm_runtime_resume(): the GPU is back, synchronously, before RM
// goes on. Both are called with RM's dynamic power mutex held (and, from rmapiPrologue, the API lock).
NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *nv) { DYNPM_LOG(nIdle, "RM indicates IDLE"); nv_xnu_idle_note(nv, true); pmPut(); return NV_OK; }
NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *nv)
{
    DYNPM_LOG(nNotIdle, "RM indicates NOT idle");
    nv_xnu_idle_note(nv, false);
    pmGet();
    return pmResumeSync("RM");
}
void NV_API_CALL nv_idle_holdoff(nv_state_t *) { DYNPM_LOG(nHoldoff, "RM idle holdoff"); pmGet(); }

// ---- Runtime suspend executed for RM (step E3, nvrmdynpm=2; docs/RTX-GC6.md) ---------------------------------------
// The Linux runtime PM pieces RM relies on, rebuilt here:
// - usage count (dev->power.usage_count): starts at 2 (pci_pm_init's forbid + the probe reference), RM's
//   nv_allow_runtime_suspend drops one, nv_indicate_idle drops one, nv_indicate_not_idle / nv_idle_holdoff /
//   nv_disallow_runtime_suspend add one; our own holders (headWatch's BAR0 reads, the revoke watchdog) add one each.
//   Zero = RM indicated idle with no holdoff: the suspend work runs kPmAutosuspendMs later (Linux autosuspend delay).
// - suspend work (rpm_suspend): under gPmLock, rechecks the count, rm_transition_dynamic_power(enter) (GC6: RM takes the
//   GPU locks itself; rmapiEnterRtd3PmPath makes its own API calls skip the dynamic power refcount). E3a stops there:
//   no D3, no power cut, so the PCI link and config space stay as they are.
// - resume (rpm_resume): under gPmLock, rm_transition_dynamic_power(exit) only. It runs in the caller's context: from
//   nv_indicate_not_idle that is RM's, with the dynamic power mutex (and maybe the API lock) held, exactly as Linux's
//   pm_runtime_resume runs inside nv_indicate_not_idle; RM's exit takes only the GPU locks.
// Lock order: RM API lock -> RM dynamic power mutex -> gPmLock -> RM GPU locks (inside rm_transition_dynamic_power).
// gMmapLock is never held while taking gPmLock (the revoke watchdog drops it first) and nothing under gPmLock takes
// gMmapLock. nv_idle_holdoff, which RM calls from inside the exit (RmTransitionDynamicPower), only touches the atomic
// count, never gPmLock. A resume asked for by the thread that is inside a transition (RM calling back) is counted and
// answered OK instead of deadlocking on gPmLock.
// Nothing else may touch the card while it is in GC6: the MSI handler skips rm_isr, the headWatch BAR0 reader holds a
// usage reference while it runs, the utilization sampler asks RM only after RM saw calls, and system sleep / shutdown
// block new suspends and bring the GPU back first (nv_xnu_dynpm_block).
namespace {
enum { kPmActive = 0, kPmSuspended = 1, kPmLost = 2 };
const uint32_t kPmAutosuspendMs = 500, kPmRetryMs = 1000, kPmMaxFailRun = 5;
IOLock *gPmLock;
volatile SInt32 gPmUsage = 2;
volatile SInt32 gPmState = kPmActive;
volatile SInt32 gPmBlock;          // system sleep, shutdown, repeated failures, lost GPU: no new suspend
const char *gPmBlockWhy = "";
thread_call_t gPmCall;
nv_state_t *gPmNv;
thread_t gPmOwner;                 // thread inside rm_transition_dynamic_power
struct {
    uint32_t entries, exits, enterFail, exitFail, tryAgain, skipped, recursion, failRun, underflow, msiOff, holds;
    uint64_t enterUsLast, enterUsMax, exitUsLast, exitUsMax, offUsTotal, offSince;
    uint32_t lastErr; const char *lastWhy;
    uint32_t d3, d3Fail, d0Fail, cfgFixes; uint16_t pmcsrOff, pmcsrIn; bool noSoftRst;
    uint32_t cuts, cutBusy, cutFail, cutNone, gonn, gonnFail; uint32_t goffMs, gonnMs, gonnMsMax; IOReturn lastCut;
} gPs;
bool gPmCut;                       // E3c: power removed by G15Fan (GOFF); a GONN is owed before anything touches the card
}

// ---- E3c: real power removal (Linux: platform _PR3 off after D3hot = D3cold) through G15Fan, kext to kext ------------
// G15Fan's callPlatformFunction("G15RtxPower", op): 0 = stop the HDMI audio function (IOPM, its driver is not ours) and
// GOFF (PG01._OFF); 1 = GONN (PG01._ON waits until the RTX answers on PCI) and audio back. No IOPM for the GPU itself.
// Without G15Fan (or an old one) nothing is cut: GC6 + D3hot as in E3b. Boot-arg -nvrmrtd3nocut = E3b on purpose.
// GONN runs inside pmResumeSync, so from nv_indicate_not_idle in RM's context: ACPI evaluation there is sleepable (RM
// holds mutexes, no spinlock; the ACPI that froze in 17/11 was the JT _DSM before RM's init, not a power resource).
static const OSSymbol *gPwrFn;
static IOService *gPwrSvc;
static bool pmCutWanted(void)
{
    static int no = -1;
    if (no < 0) { char b[4]; no = PE_parse_boot_argn("-nvrmrtd3nocut", b, sizeof b) ? 1 : 0; }
    return no == 0;
}
static IOService *pmPowerProvider(void)                  // looked up outside RM's context (suspend work), kept retained
{
    if (gPwrSvc) return gPwrSvc;
    if (!gPwrFn) gPwrFn = OSSymbol::withCString("G15RtxPower");
    OSDictionary *m = IOService::serviceMatching("G15Fan");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (IOService *sv = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr) { sv->retain(); gPwrSvc = sv; }
    OSSafeReleaseNULL(it);
    return gPwrSvc;
}

// ---- E3b: config space save/restore and D3hot around GC6 (Linux pci_pm_runtime_suspend/resume: pci_save_state,
// pci_set_power_state(D3hot); back: D0, pci_restore_state, then the driver's runtime_resume) ----------------------------
// Our own copy, not IOPCIDevice::saveDeviceState (that is IOPCIFamily's shadow for system sleep). Saved: the header
// (0x00-0x3f), the PCIe capability control registers, MSI, and the extended Resizable BAR / LTR / L1 PM Substates
// controls. Restored only where the value read after D0 differs (NoSoftRst=1 devices keep everything; counted as
// cfgFixes), in Linux's order: PCIe capability, extended caps, header from the top down, MSI last.
namespace {
struct CfgSave {
    bool valid;
    uint32_t hdr[16];
    uint8_t pcie, msi; uint16_t devCtl, lnkCtl, devCtl2, lnkCtl2;
    uint16_t msiCtl; uint32_t msiLo, msiHi; uint16_t msiData; bool msi64;
    uint16_t rbOff[6]; uint32_t rbCtl[6]; unsigned rbN;
    uint16_t ltrOff; uint32_t ltr; uint16_t l1ssOff; uint32_t l1ss1, l1ss2;
} gCfg;
const uint32_t kD3hotDelayMs = 10;   // PCI PM 1.2: 10 ms after a write to or from D3hot
}
static void cfgSave(IOPCIDevice *d)
{
    CfgSave &c = gCfg; c = {};
    for (unsigned i = 0; i < 16; i++) c.hdr[i] = d->configRead32((UInt8)(i * 4));
    UInt8 off = 0;
    if (d->findPCICapability(0x10, &off) && off) {
        c.pcie = off; c.devCtl = d->configRead16((UInt8)(off + 0x08)); c.lnkCtl = d->configRead16((UInt8)(off + 0x10));
        c.devCtl2 = d->configRead16((UInt8)(off + 0x28)); c.lnkCtl2 = d->configRead16((UInt8)(off + 0x30));
    }
    off = 0;
    if (d->findPCICapability(0x05, &off) && off) {
        c.msi = off; c.msiCtl = d->configRead16((UInt8)(off + 2)); c.msi64 = (c.msiCtl & 0x80) != 0;
        c.msiLo = d->configRead32((UInt8)(off + 4));
        if (c.msi64) { c.msiHi = d->configRead32((UInt8)(off + 8)); c.msiData = d->configRead16((UInt8)(off + 0xc)); }
        else c.msiData = d->configRead16((UInt8)(off + 8));
    }
    for (UInt32 e = 0x100, n = 0; e >= 0x100 && e < 0x1000 && n < 64; n++) {
        const UInt32 h = d->extendedConfigRead32(e);
        if (h == 0 || h == 0xffffffffU) break;
        const UInt32 id = h & 0xffff;
        if (id == 0x15 && !c.rbN) {
            const UInt32 nb = (d->extendedConfigRead32(e + 8) >> 5) & 7;
            for (UInt32 i = 0; i < nb && i < 6; i++) { c.rbOff[i] = (uint16_t)(e + 8 + i * 8); c.rbCtl[i] = d->extendedConfigRead32(c.rbOff[i]); }
            c.rbN = nb < 6 ? nb : 6;
        } else if (id == 0x18) { c.ltrOff = (uint16_t)e; c.ltr = d->extendedConfigRead32(e + 4); }
        else if (id == 0x1e) { c.l1ssOff = (uint16_t)e; c.l1ss1 = d->extendedConfigRead32(e + 8); c.l1ss2 = d->extendedConfigRead32(e + 0xc); }
        e = h >> 20;
    }
    c.valid = true;
}
static void cfgFix16(IOPCIDevice *d, UInt8 at, uint16_t want) { if (d->configRead16(at) != want) { d->configWrite16(at, want); gPs.cfgFixes++; } }
static void cfgFix32(IOPCIDevice *d, UInt8 at, uint32_t want) { if (d->configRead32(at) != want) { d->configWrite32(at, want); gPs.cfgFixes++; } }
static void cfgFixX(IOPCIDevice *d, UInt32 at, uint32_t want) { if (d->extendedConfigRead32(at) != want) { d->extendedConfigWrite32(at, want); gPs.cfgFixes++; } }
static void cfgRestore(IOPCIDevice *d)
{
    const CfgSave &c = gCfg;
    if (!c.valid) return;
    if (c.pcie) {
        cfgFix16(d, (UInt8)(c.pcie + 0x08), c.devCtl); cfgFix16(d, (UInt8)(c.pcie + 0x28), c.devCtl2);
        cfgFix16(d, (UInt8)(c.pcie + 0x30), c.lnkCtl2); cfgFix16(d, (UInt8)(c.pcie + 0x10), c.lnkCtl);
    }
    if (c.ltrOff) cfgFixX(d, c.ltrOff + 4u, c.ltr);
    if (c.l1ssOff) { cfgFixX(d, c.l1ssOff + 0xcu, c.l1ss2); cfgFixX(d, c.l1ssOff + 8u, c.l1ss1); }
    bool rbDiff = false;
    for (unsigned i = 0; i < c.rbN; i++) if ((d->extendedConfigRead32(c.rbOff[i]) & 0x3f00) != (c.rbCtl[i] & 0x3f00)) rbDiff = true;
    if (rbDiff) {                                 // resize only with memory decode off; the BARs are rewritten below
        d->configWrite16(0x04, (uint16_t)(d->configRead16(0x04) & ~0x2));
        for (unsigned i = 0; i < c.rbN; i++) cfgFixX(d, c.rbOff[i], c.rbCtl[i]);
    }
    for (int i = 15; i >= 2; i--) cfgFix32(d, (UInt8)(i * 4), c.hdr[i]);   // 0x3c .. 0x08 (BARs, cache line...), top down
    cfgFix16(d, 0x04, (uint16_t)(c.hdr[1] & 0xffff));                                   // command last (decode, bus master)
    if (c.msi) {
        cfgFix32(d, (UInt8)(c.msi + 4), c.msiLo);
        if (c.msi64) { cfgFix32(d, (UInt8)(c.msi + 8), c.msiHi); cfgFix16(d, (UInt8)(c.msi + 0xc), c.msiData); }
        else cfgFix16(d, (UInt8)(c.msi + 8), c.msiData);
        cfgFix16(d, (UInt8)(c.msi + 2), c.msiCtl);
    }
}
// PMCSR power state 0..3; false when the function has no PM capability or does not take the state.
static bool pmSetD(IOPCIDevice *d, unsigned st)
{
    if (!gPs.pmcsrOff) { UInt8 off = 0; if (!d->findPCICapability(0x01, &off) || !off) return false; gPs.pmcsrOff = off; }
    const UInt8 at = (UInt8)(gPs.pmcsrOff + 4);
    const uint16_t v = d->configRead16(at);
    gPs.noSoftRst = (v & 0x8) != 0;
    d->configWrite16(at, (uint16_t)((v & ~0x8003u) | (st & 3)));   // keep PME_En, do not clear PME_Status
    IOSleep(kD3hotDelayMs);
    gPs.pmcsrIn = d->configRead16(at);
    return (gPs.pmcsrIn & 3) == (st & 3);
}

static uint64_t pmUs(uint64_t t0) { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns); return ns / 1000ull; }
static void pmWork(thread_call_param_t, thread_call_param_t);
static void pmInit(nv_state_t *nv)
{
    if (!nv_xnu_dynpm_exec()) return;
    if (nv && !gPmNv) gPmNv = nv;
    if (!gIdlePci && nv && nv->handle) gIdlePci = ((nv_xnu_pci_slot *)nv->handle)->pci;
    if (!gPmLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gPmLock)) IOLockFree(l); }
    if (!gPmCall) { thread_call_t c = thread_call_allocate(pmWork, nullptr); if (c && !OSCompareAndSwapPtr(nullptr, c, (void * volatile *)&gPmCall)) thread_call_free(c); }
}
static void pmPublishSoon(void)
{
    if (!gIdleCall) gIdleCall = thread_call_allocate(nv_xnu_idle_publish, nullptr);
    if (gIdleCall && gIdlePci) thread_call_enter(gIdleCall);
}
static void pmSchedule(uint32_t ms)
{
    if (!nv_xnu_dynpm_exec() || !gPmCall || gPmBlock || OSAddAtomic(0, &gPmUsage) != 0) return;
    uint64_t dl; clock_interval_to_deadline(ms, kMillisecondScale, &dl);
    thread_call_enter_delayed(gPmCall, dl);
}
static void pmGet(void) { if (nv_xnu_dynpm_exec()) OSIncrementAtomic(&gPmUsage); }
static void pmPut(void)
{
    if (!nv_xnu_dynpm_exec()) return;
    for (;;) {                                         // pm_runtime_put_noidle: never below 0
        SInt32 u = gPmUsage;
        if (u <= 0) { gPs.underflow++; return; }
        if (OSCompareAndSwap((UInt32)u, (UInt32)(u - 1), (volatile UInt32 *)&gPmUsage)) { if (u == 1) pmSchedule(kPmAutosuspendMs); return; }
    }
}
// Suspend work (thread call): RM indicated idle and nothing holds the GPU.
static void pmWork(thread_call_param_t, thread_call_param_t)
{
    if (!gPmLock) return;
    bool again = false; NV_STATUS st = NV_OK; uint64_t us = 0; bool did = false;
    IOLockLock(gPmLock);
    if (OSAddAtomic(0, &gPmUsage) != 0 || gPmBlock || gPmState != kPmActive || !gPmNv) gPs.skipped++;
    else {
        NvBool tryAgain = NV_FALSE;
        const uint64_t t0 = mach_absolute_time();
        gPmOwner = current_thread();
        st = rm_transition_dynamic_power(NULL, gPmNv, NV_TRUE, &tryAgain);
        gPmOwner = THREAD_NULL;
        us = pmUs(t0); did = true;
        if (st == NV_OK) {
            gPmState = kPmSuspended; gPs.entries++; gPs.failRun = 0;
            gPs.enterUsLast = us; if (us > gPs.enterUsMax) gPs.enterUsMax = us;
            gPs.offSince = mach_absolute_time();
            // E3b: config space saved, then D3hot (the GPU's GC6 entry is done, nothing of RM touches it any more)
            if (gIdlePci) { cfgSave(gIdlePci); if (pmSetD(gIdlePci, 3)) gPs.d3++; else gPs.d3Fail++; }
            // E3c: power off (only from D3hot, as Linux goes D3hot -> platform D3cold)
            IOService *pw = (pmCutWanted() && gIdlePci && (gPs.pmcsrIn & 3) == 3) ? pmPowerProvider() : nullptr;
            if (pw && gPwrFn) {
                uint32_t ms = 0;
                const IOReturn r = pw->callPlatformFunction(gPwrFn, true, (void *)(uintptr_t)0, &ms, nullptr, nullptr);
                gPs.lastCut = r; gPs.goffMs = ms;
                if (r == kIOReturnSuccess) { gPmCut = true; gPs.cuts++; }
                else if (r == kIOReturnBusy) gPs.cutBusy++;      // HDMI audio would not stop: GC6 + D3hot only
                else gPs.cutFail++;
            } else if (pmCutWanted()) gPs.cutNone++;
        } else {
            gPs.lastErr = (uint32_t)st;
            if (tryAgain) { gPs.tryAgain++; again = true; }
            else if (++gPs.enterFail, ++gPs.failRun >= kPmMaxFailRun) { gPmBlock = 1; gPmBlockWhy = "entry failed repeatedly"; }
        }
    }
    IOLockUnlock(gPmLock);
    if (did && (gPs.entries + gPs.enterFail <= 50 || (gPs.entries % 100) == 0 || st != NV_OK))
        IOLog("NVRM-xnu: rtd3: GC6 entry %s (0x%x) in %llu.%03llu ms (entries %u, failures %u%s)\n", st == NV_OK ? "done" : "FAILED",
              (unsigned)st, us / 1000, us % 1000, gPs.entries, gPs.enterFail, again ? ", RM asks to try again" : gPmBlock ? ", suspend now blocked" : "");
    if (again) pmSchedule(kPmRetryMs);
    if (did) pmPublishSoon();
}
// rpm_resume: the GPU is at full power when this returns NV_OK.
static NV_STATUS pmResumeSync(const char *why)
{
    if (!nv_xnu_dynpm_exec() || !gPmLock) return NV_OK;
    if (gPmOwner == current_thread()) { gPs.recursion++; return NV_OK; }
    NV_STATUS st = NV_OK; uint64_t us = 0, off = 0; bool did = false;
    IOLockLock(gPmLock);
    if (gPmState == kPmSuspended && gPmNv) {
        NvBool tryAgain = NV_FALSE;
        const uint64_t t0 = mach_absolute_time();
        gPmOwner = current_thread();
        // E3b: D0 and the saved config back before RM's exit touches the GPU
        bool gone = false;
        if (gPmCut && gPwrSvc && gPwrFn) {                 // E3c: power back first (synchronous: PGON waits for the link)
            uint32_t ms = 0;
            const IOReturn r = gPwrSvc->callPlatformFunction(gPwrFn, true, (void *)(uintptr_t)1, &ms, nullptr, nullptr);
            gPs.gonnMs = ms; if (ms > gPs.gonnMsMax) gPs.gonnMsMax = ms;
            if (r == kIOReturnSuccess) { gPmCut = false; gPs.gonn++; } else gPs.gonnFail++;
        }
        if (gIdlePci) {
            if (!pmSetD(gIdlePci, 0)) gPs.d0Fail++;
            gone = gIdlePci->configRead16(0x00) != 0x10de;   // not on the bus: RM must not touch it
            if (!gone) cfgRestore(gIdlePci);
        }
        st = gone ? NV_ERR_GPU_IS_LOST : rm_transition_dynamic_power(NULL, gPmNv, NV_FALSE, &tryAgain);
        gPmOwner = THREAD_NULL;
        us = pmUs(t0); did = true;
        absolutetime_to_nanoseconds(t0 - gPs.offSince, &off); off /= 1000ull;
        gPs.offUsTotal += off; gPs.lastWhy = why;
        if (st == NV_OK) { gPmState = kPmActive; gPs.exits++; gPs.exitUsLast = us; if (us > gPs.exitUsMax) gPs.exitUsMax = us; }
        else { gPmState = kPmLost; gPs.exitFail++; gPs.lastErr = (uint32_t)st; gPmBlock = 1; gPmBlockWhy = "exit failed (GPU lost)"; }
    } else if (gPmState == kPmLost) st = NV_ERR_GPU_IS_LOST;
    IOLockUnlock(gPmLock);
    if (did && (gPs.exits + gPs.exitFail <= 50 || (gPs.exits % 100) == 0 || st != NV_OK))
        IOLog("NVRM-xnu: rtd3: GC6 exit for %s %s (0x%x) in %llu.%03llu ms after %llu ms in GC6 (exits %u)\n", why,
              st == NV_OK ? "done" : "FAILED", (unsigned)st, us / 1000, us % 1000, off / 1000, gPs.exits);
    if (did) pmPublishSoon();
    return st;
}

// True while the card must not be touched outside RM (GC6 or lost). Lock-free read for the MSI handler.
extern "C" bool nv_xnu_dynpm_gpu_off(void)
{
    if (!nv_xnu_dynpm_exec() || gPmState == kPmActive) return false;
    OSIncrementAtomic((volatile SInt32 *)&gPs.msiOff);
    return true;
}
extern "C" bool nv_xnu_dynpm_gpu_off_quiet(void) { return nv_xnu_dynpm_exec() && gPmState != kPmActive; }
// GC6 exits (and failed ones) so far: a caller timing an RM call can tell that a slow call waited for the GPU to come
// back (0.4 s without power removal, ~1.2 s with it) instead of a hung firmware.
extern "C" uint32_t nv_xnu_dynpm_exit_count(void) { return nv_xnu_dynpm_exec() ? gPs.exits + gPs.exitFail : 0; }
// A kernel reader of the card's registers outside RM (headWatch): hold = usage reference + GPU back if suspended.
extern "C" void nv_xnu_dynpm_hold(bool on, const char *why)
{
    if (!nv_xnu_dynpm_exec()) return;
    if (on) { pmGet(); gPs.holds++; pmResumeSync(why); } else pmPut();
}
// System sleep / shutdown / unload: no new suspend, GPU back now (outside RM's dynamic power mutex, as the PCI core's
// pm_runtime_resume before system suspend on Linux; RM's state may still say idle-indicated, its next reference then
// finds the GPU active). Unblocked after a good wake.
extern "C" void nv_xnu_dynpm_block(bool on, const char *why)
{
    if (!nv_xnu_dynpm_exec() || !gPmLock) return;
    if (on) {
        IOLockLock(gPmLock); gPmBlock = 1; gPmBlockWhy = why; IOLockUnlock(gPmLock);
        if (gPmCall) thread_call_cancel(gPmCall);   // a running pmWork sees gPmBlock under gPmLock (cancel_wait is not exported to kexts)
        pmResumeSync(why);
    } else {
        IOLockLock(gPmLock);
        if (gPmState == kPmActive) { gPmBlock = 0; gPmBlockWhy = ""; gPs.failRun = 0; }
        IOLockUnlock(gPmLock);
        pmSchedule(kPmAutosuspendMs);
    }
    pmPublishSoon();
}
static void nv_xnu_rtd3_publish(IORegistryEntry *where)
{
    if (!nv_xnu_dynpm_exec() || !where) return;
    const SInt32 stt = gPmState;
    uint64_t offNow = 0;
    if (stt == kPmSuspended) { absolutetime_to_nanoseconds(mach_absolute_time() - gPs.offSince, &offNow); offNow /= 1000ull; }
    char b[1000];
    snprintf(b, sizeof b, "state %s | usage %d | block %s | entries %u exits %u | fail enter %u exit %u tryAgain %u | "
             "enter ms last %llu.%03llu max %llu.%03llu | exit ms last %llu.%03llu max %llu.%03llu | GC6 s total %llu now %llu | "
             "last exit by %s | skipped %u recursion %u underflow %u msiOff %u holds %u | lastErr 0x%x | "
             "D3hot %u fail %u D0fail %u pmcsr 0x%04x noSoftRst %s cfgFixes %u | cut %s: cuts %u busy %u fail %u none %u "
             "GONN %u fail %u | GOFF ms %u GONN ms last %u max %u | lastCut 0x%x",
             stt == kPmActive ? "active" : stt == kPmSuspended ? "GC6" : "LOST", (int)gPmUsage, gPmBlock ? gPmBlockWhy : "no",
             gPs.entries, gPs.exits, gPs.enterFail, gPs.exitFail, gPs.tryAgain,
             gPs.enterUsLast / 1000, gPs.enterUsLast % 1000, gPs.enterUsMax / 1000, gPs.enterUsMax % 1000,
             gPs.exitUsLast / 1000, gPs.exitUsLast % 1000, gPs.exitUsMax / 1000, gPs.exitUsMax % 1000,
             (gPs.offUsTotal + offNow) / 1000000ull, offNow / 1000000ull, gPs.lastWhy ? gPs.lastWhy : "-",
             gPs.skipped, gPs.recursion, gPs.underflow, gPs.msiOff, gPs.holds, gPs.lastErr,
             gPs.d3, gPs.d3Fail, gPs.d0Fail, gPs.pmcsrIn, gPs.noSoftRst ? "yes" : "no", gPs.cfgFixes,
             !pmCutWanted() ? "off (-nvrmrtd3nocut)" : gPmCut ? "POWER OFF" : "powered", gPs.cuts, gPs.cutBusy, gPs.cutFail,
             gPs.cutNone, gPs.gonn, gPs.gonnFail, gPs.goffMs, gPs.gonnMs, gPs.gonnMsMax, (unsigned)gPs.lastCut);
    where->setProperty("nvrm-rtd3", b);
    where->setProperty("NVRMRtd3", kOSBooleanTrue);
    where->setProperty("NVRMGC6", stt == kPmSuspended ? kOSBooleanTrue : kOSBooleanFalse);
    where->setProperty("NVRMGC6Count", (unsigned long long)gPs.entries, 32);
    where->setProperty("NVRMRtd3Cut", gPmCut ? kOSBooleanTrue : kOSBooleanFalse);
}

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
    nv_xnu_rtd3_publish(where);   // E3: the time in GC6 moves with the sampler's periodic refresh too
    nv_xnu_rv_publish(where);     // E4: the poll counters too (measured over two refreshes)
}

// Whether the utilization sample should be skipped. The sampler asks RM once a second; each RM API call takes a dynamic
// power reference and puts the state back to in-use, so RM never saw two idle checks (5 s each) in a row and never
// indicated idle (like nvidia-smi polling on Linux). But in fine mode RM sits in idle-instant between calls even while
// a game draws, so "skip while idle-instant or more" showed a game at 0 % (docs/RTX-GC6.md, 21/11 22:55); and work
// submitted only through the USERMODE doorbell (rtxstress, NVK) does not call RM at all, so "skip while RM's refs did
// not move" still showed it at 0 % (E3c, 21/11 23:30). Skip only when ALL of these hold:
//   - RM counts the GPU idle (fine mode, idle-instant or more);
//   - nobody referenced RM since the end of the previous sample (refs c+f unchanged);
//   - no revoked mapping was touched since then (faults unchanged: a doorbell write after a revoke is work);
//   - RM already indicated idle (the card may be in GC6: never wake it for a number), or the previous real sample is
//     less than kSampleEveryMs old. From a sample to RM's idle indication takes 5-10 s (two idle checks), so one
//     sample every 12 s still lets a quiet GPU reach GC6, and doorbell-only work gets a reading every 12 s.
static volatile uint32_t gSampleRefs, gSampleFaults; static volatile bool gSampleRefsOk; static volatile uint64_t gSampleAt;
static const uint32_t kSampleEveryMs = 12000;
static uint32_t rvFaultCount(void);
// 0 = sample now; 1 = skip, answer with the last measured value (the GPU may still work through the doorbell; NVAccel
// drops a value older than 5 s, shorter than RM's 5-10 s idle path); 2 = skip, RM indicated idle (in GC6 with
// nvrmdynpm=2): answer 0 %.
extern "C" int nv_xnu_dynpm_sample_skip(nv_state_t *nv)
{
    if (!nv_xnu_dynpm_on() || !nv) return 0;
    NvU32 v[22] = {};
    rm_xnu_dynamic_power_snapshot(nv, v, NULL);
    const bool idle = v[0] == 2 /* fine */ && v[1] >= 2 /* idle-instant, idle-sustained, idle-indicated */;
    if (!idle || !gSampleRefsOk || (uint32_t)(v[9] + v[10]) != gSampleRefs || rvFaultCount() != gSampleFaults) return 0;
    if (v[1] == 4 /* idle-indicated */) return 2;
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - gSampleAt, &ns);
    return ns < (uint64_t)kSampleEveryMs * 1000000ull ? 1 : 0;
}
// After each real sample: what the next skip decision compares with (the sample's own calls included).
extern "C" void nv_xnu_dynpm_sample_done(nv_state_t *nv)
{
    if (!nv_xnu_dynpm_on() || !nv) return;
    NvU32 v[22] = {};
    rm_xnu_dynamic_power_snapshot(nv, v, NULL);
    gSampleRefs = (uint32_t)(v[9] + v[10]); gSampleFaults = rvFaultCount(); gSampleAt = mach_absolute_time(); gSampleRefsOk = true;
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
// Watchdog: a fault not served in kRvWatchdogMs is restored anyway (logged and counted). With nvrmdynpm=1 nothing is
// powered off, so the pages are valid. With nvrmdynpm=2 the card may be in GC6: the watchdog first brings it back
// (usage reference + synchronous resume, gMmapLock dropped), restores only if that worked, and keeps the reference
// until RM says safe-to-mmap again (its own wakeup arrived). Gate: nvrmdynpm>=1. With 0 nothing here runs and
// clientMemoryForType is as before.
// Lock order: RM dynamic power mutex -> gMmapLock -> gIOMemoryLock. Nothing here calls RM with gMmapLock held.
// Counters: nvrm-revoke on the RTX PCI entry.
#include <IOKit/IOMemoryDescriptor.h>

namespace {
struct RvEntry { IOMemoryDescriptor *md; bool redirected; bool multi; };
const unsigned kRvMax = 4096;
// Poll period: 15 ms while the card is powered (revoked, GC6 not entered yet: a fault must not wait long); 100 ms while
// it is in GC6 (E4: ~66 -> ~10 CPU wakeups/s for all the idle time; the way back already costs ~1.24 s with the power
// cut, so the detection adds at most 0.1 s). The KPI has no thread call with leeway, so the count is what can shrink.
const uint32_t kRvPollMs = 15, kRvPollOffMs = 100, kRvWatchdogMs = 2000;
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
bool gRvDogHold;              // the watchdog holds a usage reference (nvrmdynpm=2) until RM sets safe again
struct { uint32_t revokes, redirects, restores, faultsSafe, faultsWake, wakeReqs, wakeFail, watchdog, dogWakeFail, newBlocked, refused;
         uint64_t lastFaultPhys, polls, pollsOff, pollNs; } gRs;
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
    const bool off = nv_xnu_dynpm_gpu_off_quiet();       // GC6 (or lost): slow poll
    if (off) gRs.pollsOff++;
    uint64_t dl; clock_interval_to_deadline(off ? kRvPollOffMs : kRvPollMs, kMillisecondScale, &dl);
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
    bool wake = false, faulted = false, dog = false, dogWake = false;
    nv_state_t *nv = nullptr;
    const uint64_t pt0 = mach_absolute_time();
    IOLockLock(gMmapLock);
    gRvPollArmed = 0;
    gRs.polls++;
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
        if (ns > (uint64_t)kRvWatchdogMs * 1000000ull) {
            gRs.watchdog++; dog = true;
            if (!nv_xnu_dynpm_exec()) rvRestoreAllLocked();
            else if (!gRvDogHold) { gRvDogHold = true; dogWake = true; }   // power first, below, without gMmapLock
            else rvRestoreAllLocked();                                      // already held awake by an earlier watchdog
        }
    }
    rvPollArmLocked();
    IOLockUnlock(gMmapLock);
    if (dogWake) {
        nv_xnu_dynpm_hold(true, "revoke watchdog");                         // usage reference + synchronous GC6 exit
        IOLockLock(gMmapLock);
        if (!nv_xnu_dynpm_gpu_off_quiet()) rvRestoreAllLocked(); else gRs.dogWakeFail++;
        IOLockUnlock(gMmapLock);
    }
    if (dog) IOLog("NVRM-xnu: dynpm: revoke WATCHDOG: a fault waited %u ms for RM, %s (#%u)\n", kRvWatchdogMs,
                   dogWake ? "GPU brought back by the watchdog, mappings restored" : "mappings restored", gRs.watchdog);
    if (wake) rvWakeGpu(nv);
    if (faulted || dog) rvPublishSoon();
    gRs.pollNs += pmUs(pt0) * 1000ull;
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
    if (safe && gRvDogHold) { gRvDogHold = false; nv_xnu_dynpm_hold(false, "revoke watchdog"); }   // RM's wakeup arrived
    if (safe && gRvRedirected && gRvRestoreCall) thread_call_enter(gRvRestoreCall);
}

static uint32_t rvFaultCount(void) { return gRs.faultsSafe + gRs.faultsWake + gRs.newBlocked; }
static void nv_xnu_rv_publish(IORegistryEntry *where)
{
    if (!nv_xnu_dynpm_on() || !where) return;
    char b[600];
    snprintf(b, sizeof b, "tracked %u | redirected %u | multi %u | overflow %u | safe %s | allRevoked %s | revokes %u refused %u "
             "redirects %u restores %u | faults safe %u wake %u | wakeReq %u fail %u | watchdog %u dogWakeFail %u | newBlocked %u | "
             "lastFault 0x%llx | polls %llu (in GC6 %llu) cpu us %llu",
             gRvN, gRvRedirected, gRvMulti, gRvOverflow, gRvSafe ? "yes" : "NO", gRvAllRevoked ? "yes" : "no", gRs.revokes,
             gRs.refused, gRs.redirects, gRs.restores, gRs.faultsSafe, gRs.faultsWake, gRs.wakeReqs, gRs.wakeFail,
             gRs.watchdog, gRs.dogWakeFail, gRs.newBlocked, (unsigned long long)gRs.lastFaultPhys,
             (unsigned long long)gRs.polls, (unsigned long long)gRs.pollsOff, (unsigned long long)(gRs.pollNs / 1000ull));
    where->setProperty("nvrm-revoke", b);
}

#include <inttypes.h>

#include "common/hos_log.h"
#include "cpu_internal.h"
#include "patch/patch.h"

#define MOD "svc"

typedef struct svc_entry {
    const char *name;
    hos_svc_fn fn;
} svc_entry;

/* Names follow switchbrew. fn == NULL means "known, not implemented". */
static const svc_entry g_table[HOS_SVC_MAX + 1] = {
    [0x01] = {"SetHeapSize", svc_set_heap_size},
    [0x02] = {"SetMemoryPermission", svc_set_memory_permission},
    [0x03] = {"SetMemoryAttribute", svc_set_memory_attribute},
    [0x04] = {"MapMemory", svc_map_memory},
    [0x05] = {"UnmapMemory", svc_unmap_memory},
    [0x06] = {"QueryMemory", svc_query_memory},
    [0x07] = {"ExitProcess", svc_exit_process},
    [0x08] = {"CreateThread", svc_create_thread},
    [0x09] = {"StartThread", svc_start_thread},
    [0x0A] = {"ExitThread", svc_exit_thread},
    [0x0B] = {"SleepThread", svc_sleep_thread},
    [0x0C] = {"GetThreadPriority", svc_get_thread_priority},
    [0x0D] = {"SetThreadPriority", svc_set_thread_priority},
    [0x0E] = {"GetThreadCoreMask", svc_get_thread_core_mask},
    [0x0F] = {"SetThreadCoreMask", svc_set_thread_core_mask},
    [0x10] = {"GetCurrentProcessorNumber", svc_get_current_processor_number},
    [0x11] = {"SignalEvent", svc_signal_event},
    [0x12] = {"ClearEvent", svc_clear_event},
    [0x13] = {"MapSharedMemory", svc_map_shared_memory},
    [0x14] = {"UnmapSharedMemory", svc_unmap_shared_memory},
    [0x15] = {"CreateTransferMemory", svc_create_transfer_memory},
    [0x16] = {"CloseHandle", svc_close_handle},
    [0x17] = {"ResetSignal", svc_reset_signal},
    [0x18] = {"WaitSynchronization", svc_wait_synchronization},
    [0x19] = {"CancelSynchronization", svc_cancel_synchronization},
    [0x1A] = {"ArbitrateLock", svc_arbitrate_lock},
    [0x1B] = {"ArbitrateUnlock", svc_arbitrate_unlock},
    [0x1C] = {"WaitProcessWideKeyAtomic", svc_wait_process_wide_key_atomic},
    [0x1D] = {"SignalProcessWideKey", svc_signal_process_wide_key},
    [0x1E] = {"GetSystemTick", svc_get_system_tick},
    [0x1F] = {"ConnectToNamedPort", svc_connect_to_named_port},
    [0x20] = {"SendSyncRequestLight", NULL},
    [0x21] = {"SendSyncRequest", svc_send_sync_request},
    [0x22] = {"SendSyncRequestWithUserBuffer", NULL},
    [0x23] = {"SendAsyncRequestWithUserBuffer", NULL},
    [0x24] = {"GetProcessId", svc_get_process_id},
    [0x25] = {"GetThreadId", svc_get_thread_id},
    [0x26] = {"Break", svc_break},
    [0x27] = {"OutputDebugString", svc_output_debug_string},
    [0x28] = {"ReturnFromException", NULL},
    [0x29] = {"GetInfo", svc_get_info},
    [0x2A] = {"FlushEntireDataCache", svc_flush_cache_noop},
    [0x2B] = {"FlushDataCache", svc_flush_cache_noop},
    [0x2C] = {"MapPhysicalMemory", NULL},
    [0x2D] = {"UnmapPhysicalMemory", NULL},
    [0x2E] = {"GetDebugFutureThreadInfo", NULL},
    [0x2F] = {"GetLastThreadInfo", NULL},
    [0x30] = {"GetResourceLimitLimitValue", NULL},
    [0x31] = {"GetResourceLimitCurrentValue", NULL},
    [0x32] = {"SetThreadActivity", NULL},
    [0x33] = {"GetThreadContext3", NULL},
    [0x34] = {"WaitForAddress", svc_wait_for_address},
    [0x35] = {"SignalToAddress", svc_signal_to_address},
    [0x36] = {"SynchronizePreemptionState", NULL},
    [0x37] = {"GetResourceLimitPeakValue", NULL},
    [0x39] = {"CreateIoPool", NULL},
    [0x3A] = {"CreateIoRegion", NULL},
    [0x3C] = {"KernelDebug", NULL},
    [0x3D] = {"ChangeKernelTraceState", NULL},
    [0x40] = {"CreateSession", NULL},
    [0x41] = {"AcceptSession", NULL},
    [0x42] = {"ReplyAndReceiveLight", NULL},
    [0x43] = {"ReplyAndReceive", NULL},
    [0x44] = {"ReplyAndReceiveWithUserBuffer", NULL},
    [0x45] = {"CreateEvent", svc_create_event},
    [0x46] = {"MapIoRegion", NULL},
    [0x47] = {"UnmapIoRegion", NULL},
    [0x48] = {"MapPhysicalMemoryUnsafe", NULL},
    [0x49] = {"UnmapPhysicalMemoryUnsafe", NULL},
    [0x4A] = {"SetUnsafeLimit", NULL},
    [0x4B] = {"CreateCodeMemory", NULL},
    [0x4C] = {"ControlCodeMemory", NULL},
    [0x4D] = {"SleepSystem", NULL},
    [0x4E] = {"ReadWriteRegister", NULL},
    [0x4F] = {"SetProcessActivity", NULL},
    [0x50] = {"CreateSharedMemory", svc_create_shared_memory},
    [0x51] = {"MapTransferMemory", NULL},
    [0x52] = {"UnmapTransferMemory", NULL},
    [0x53] = {"CreateInterruptEvent", NULL},
    [0x54] = {"QueryPhysicalAddress", NULL},
    [0x55] = {"QueryIoMapping", NULL},
    [0x56] = {"CreateDeviceAddressSpace", NULL},
    [0x57] = {"AttachDeviceAddressSpace", NULL},
    [0x58] = {"DetachDeviceAddressSpace", NULL},
    [0x59] = {"MapDeviceAddressSpaceByForce", NULL},
    [0x5A] = {"MapDeviceAddressSpaceAligned", NULL},
    [0x5B] = {"MapDeviceAddressSpace", NULL},
    [0x5C] = {"UnmapDeviceAddressSpace", NULL},
    [0x5D] = {"InvalidateProcessDataCache", NULL},
    [0x5E] = {"StoreProcessDataCache", NULL},
    [0x5F] = {"FlushProcessDataCache", NULL},
    [0x60] = {"DebugActiveProcess", NULL},
    [0x61] = {"BreakDebugProcess", NULL},
    [0x62] = {"TerminateDebugProcess", NULL},
    [0x63] = {"GetDebugEvent", NULL},
    [0x64] = {"ContinueDebugEvent", NULL},
    [0x65] = {"GetProcessList", NULL},
    [0x66] = {"GetThreadList", NULL},
    [0x67] = {"GetDebugThreadContext", NULL},
    [0x68] = {"SetDebugThreadContext", NULL},
    [0x69] = {"QueryDebugProcessMemory", NULL},
    [0x6A] = {"ReadDebugProcessMemory", NULL},
    [0x6B] = {"WriteDebugProcessMemory", NULL},
    [0x6C] = {"SetHardwareBreakPoint", NULL},
    [0x6D] = {"GetDebugThreadParam", NULL},
    [0x6F] = {"GetSystemInfo", NULL},
    [0x70] = {"CreatePort", NULL},
    [0x71] = {"ManageNamedPort", NULL},
    [0x72] = {"ConnectToPort", NULL},
    [0x73] = {"SetProcessMemoryPermission", NULL},
    [0x74] = {"MapProcessMemory", NULL},
    [0x75] = {"UnmapProcessMemory", NULL},
    [0x76] = {"QueryProcessMemory", NULL},
    [0x77] = {"MapProcessCodeMemory", NULL},
    [0x78] = {"UnmapProcessCodeMemory", NULL},
    [0x79] = {"CreateProcess", NULL},
    [0x7A] = {"StartProcess", NULL},
    [0x7B] = {"TerminateProcess", NULL},
    [0x7C] = {"GetProcessInfo", NULL},
    [0x7D] = {"CreateResourceLimit", NULL},
    [0x7E] = {"SetResourceLimitLimitValue", NULL},
    [0x7F] = {"CallSecureMonitor", NULL},
};

void hos_svc_dispatch(uint32_t id, hos_svc_regs *r) {
    const svc_entry *e = (id <= HOS_SVC_MAX) ? &g_table[id] : NULL;
    const char *name = (e && e->name) ? e->name : "?";

    if (hos_log_get_level() >= HOS_LOG_TRACE) {
        HOS_LOGT(MOD,
                 "svc %#04x %s(x0=%#" PRIx64 ", x1=%#" PRIx64 ", x2=%#" PRIx64 ", x3=%#" PRIx64 ")",
                 id, name, r->x[0], r->x[1], r->x[2], r->x[3]);
    }
    if (!e || !e->fn) {
        /* FIXME(#3): synchronisation and IPC svcs land here until Phase 1b. */
        HOS_LOGW(MOD, "unimplemented svc %#04x %s (x0=%#" PRIx64 ", x1=%#" PRIx64 ")", id, name,
                 r->x[0], r->x[1]);
        r->x[0] = HOS_KERNEL_NOT_IMPLEMENTED;
        return;
    }
    e->fn(r);
    if (hos_log_get_level() >= HOS_LOG_TRACE) {
        HOS_LOGT(MOD, "  -> x0=%#" PRIx64 " x1=%#" PRIx64, r->x[0], r->x[1]);
    }
}

// iSCSIKit virtual HBA. Queues SCSI tasks from the kernel and hands them to
// iscsikitd through iSCSIKitUserClient; the daemon executes them over iSCSI
// and completes them back here.

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSData.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOUserClient.h>
#include <time.h>
#include <SCSIControllerDriverKit/IOSCSIParallelControllerCharacteristics.h>
#include "iSCSIKitDext.h"
#include "iSCSIKitUserClient.h"
#include "iSCSIKitProtocol.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "iSCSIKitDext: " fmt, ##__VA_ARGS__)

static constexpr uint32_t kMaxTaskCount = 64;
static_assert(kMaxTaskCount == ISCSIKIT_MAX_TASKS, "protocol header out of sync");
// Max bytes per task. Kept in lockstep with the daemon's dequeue/complete
// buffers (ISCSIKIT_MAX_TRANSFER) so a single struct round trip always fits.
// A larger transfer amortizes the fixed per-task dext<->daemon IPC cost over
// more data: at ~10 ms/op that overhead dominated when tasks were 16 KiB.
static constexpr uint64_t kMaxTransferSize = ISCSIKIT_MAX_TRANSFER;
// Largest transfer one task can carry: one physically contiguous page (see
// the constraints in UserInitializeController).
static constexpr uint64_t kMaxTaskBytes = 16384;

namespace {

enum class SlotState : uint8_t {
    free_ = 0,
    pending,   // queued, daemon not yet notified of completion
    inFlight,  // daemon dequeued it
};

struct TaskSlot {
    SlotState state;
    SCSIUserParallelTask task;
    OSAction * completion;
    // Data buffer for the task, fetched in UserProcessParallelTask — the
    // only context where UserGetDataBuffer is legal to call.
    IOBufferMemoryDescriptor * buffer;
    uint64_t bufferAddress;
    uint64_t bufferLength;
    // For writes: the task's fBufferIOVMAddr. On a virtual controller the
    // "IOVM" space is the dext's own address space, so outbound data is
    // readable here directly; the bounce buffer from UserGetDataBuffer is
    // never pre-filled for writes.
    uint64_t bufferIOVA;
    uint16_t firstNonzeroOffset;
    // Write payload captured inside UserProcessParallelTask (the documented
    // context for UserGetDataBuffer) so later stages never touch the buffer.
    uint8_t * staged;
    uint64_t stagedLength;
    // Mapping of the task's data buffer created with CreateMapping. The
    // documented way to reach an IOMemoryDescriptor's bytes; GetAddressRange
    // is LOCALONLY and only meaningful for a descriptor the dext created.
    IOMemoryMap * map;
    // Diagnostics: when the slot entered `pending` (CLOCK_UPTIME_RAW ns),
    // and the dext sequence number of the task it holds.
    uint64_t queuedAtNs;
    uint64_t seq;
};

static uint64_t nowNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

}  // namespace

struct iSCSIKitDext_IVars {
    IOLock * lock;
    iSCSIKitUserClient * userClient;  // not retained; cleared on client Stop
    uint64_t activeTaskIDs;  // bitmap, task IDs 1..64; duplicate detection
    TaskSlot slots[kMaxTaskCount];
    // Target create/destroy run here, never on an RPC dispatch queue:
    // UserCreateTargetForID blocks until the kernel finishes probing the
    // target, and the probe's own callbacks need the RPC queues free.
    IODispatchQueue * targetOpsQueue;
    // Write-path probe, published to the IORegistry (ioreg -r -n iSCSIKitDext -l).
    // Compares what the two ways of reaching the data buffer actually contain,
    // because dext os_log output is not visible on this system.
    uint64_t probeWrites, probeRangeRC, probeRangeLength, probeRangeNonzero;
    uint64_t probeMapRC, probeMapLength, probeMapNonzero, probeRequested;
    // Cumulative, so one good write cannot hide a zeroed one.
    uint64_t probeZeroPayloadWrites, probeTotalBytes, probeTotalNonzero, probeMismatches;
    // Bytes that actually differ between the two routes (memcmp, not counts).
    uint64_t probeRouteDiffBytes;
    // Last kProbeLog write tasks, so a raw write and a formatting write can be
    // told apart instead of being averaged together.
    static constexpr uint32_t kProbeLog = 32;
    uint64_t logOpcode[kProbeLog], logLBA[kProbeLog], logLength[kProbeLog];
    uint64_t logRangeNZ[kProbeLog], logMapNZ[kProbeLog], logDiff[kProbeLog];
    uint32_t logIndex, logCount;
    // Task lifecycle counters (lost-notification diagnostics), published to
    // the IORegistry as TaskStats_* every 32 received / 32 completed tasks
    // and on every ListPending call from the daemon (1 Hz).
    uint64_t statReceived, statNotified, statNotifyFailed, statDequeued;
    uint64_t statCompleted, statMaxPending, statListPendingReported;
    // Event trace (see ISCSIKitEventType). Ring of ISCSIKIT_EVENT_RING
    // entries; ringNext counts every event ever written, so the reader can
    // tell how many it lost. Guarded by `lock`.
    ISCSIKitEvent * ring;
    uint64_t ringNext;
    uint64_t nextSeq;
};

// Appends one trace event. Caller holds v->lock.
static void recordEventLocked(iSCSIKitDext_IVars * v, uint8_t type, uint64_t seq,
                              uint64_t taskID, uint32_t code, uint64_t value,
                              uint8_t extra = 0)
{
    if (!v->ring) {
        return;
    }
    ISCSIKitEvent & e = v->ring[v->ringNext % ISCSIKIT_EVENT_RING];
    e.seq = seq;
    e.timeNs = nowNs();
    e.taskID = taskID;
    e.value = value;
    e.code = code;
    e.type = type;
    e.extra = extra;
    e.reserved[0] = e.reserved[1] = 0;
    v->ringNext++;
}

static void recordEvent(iSCSIKitDext_IVars * v, uint8_t type, uint64_t seq,
                        uint64_t taskID, uint32_t code, uint64_t value,
                        uint8_t extra = 0)
{
    IOLockLock(v->lock);
    recordEventLocked(v, type, seq, taskID, code, value, extra);
    IOLockUnlock(v->lock);
}

// Publishes the task lifecycle counters. Takes the lock only to snapshot;
// SetProperties (a blocking RPC) runs outside it.
static void publishTaskStats(iSCSIKitDext * dext, iSCSIKitDext_IVars * v)
{
    uint64_t received, notified, notifyFailed, dequeued, completed, maxPending;
    uint64_t listReported, pending = 0, inFlight = 0, oldestID = 0, oldestAgeMs = 0;
    uint64_t now = nowNs();
    IOLockLock(v->lock);
    received = v->statReceived;
    notified = v->statNotified;
    notifyFailed = v->statNotifyFailed;
    dequeued = v->statDequeued;
    completed = v->statCompleted;
    maxPending = v->statMaxPending;
    listReported = v->statListPendingReported;
    uint64_t oldestAt = UINT64_MAX;
    for (auto & slot : v->slots) {
        if (slot.state == SlotState::pending) {
            pending++;
            if (slot.queuedAtNs < oldestAt) {
                oldestAt = slot.queuedAtNs;
                oldestID = slot.task.fControllerTaskIdentifier;
            }
        } else if (slot.state == SlotState::inFlight) {
            inFlight++;
        }
    }
    IOLockUnlock(v->lock);
    if (pending > 0 && now > oldestAt) {
        oldestAgeMs = (now - oldestAt) / 1000000;
    }

    OSDictionary * d = OSDictionary::withCapacity(12);
    if (!d) {
        return;
    }
    auto set = [&](const char * key, uint64_t value) {
        OSNumber * n = OSNumber::withNumber(value, 64);
        if (n) {
            d->setObject(key, n);
            n->release();
        }
    };
    set("TaskStats_Received", received);
    set("TaskStats_Notified", notified);
    set("TaskStats_NotifyFailed", notifyFailed);
    set("TaskStats_Dequeued", dequeued);
    set("TaskStats_Completed", completed);
    set("TaskStats_CurrentPending", pending);
    set("TaskStats_CurrentInFlight", inFlight);
    set("TaskStats_MaxPending", maxPending);
    set("TaskStats_OldestPendingTaskID", oldestID);
    set("TaskStats_OldestPendingAgeMs", oldestAgeMs);
    set("TaskStats_StalePendingReported", listReported);
    dext->SetProperties(d);
    d->release();
}

// Publishes the write probe. Only called for write tasks (a blocking RPC, so
// never on the read hot path).
static void publishWriteProbe(iSCSIKitDext * dext, iSCSIKitDext_IVars * v)
{
    OSDictionary * d = OSDictionary::withCapacity(8);
    if (!d) {
        return;
    }
    auto set = [&](const char * key, uint64_t value) {
        OSNumber * n = OSNumber::withNumber(value, 64);
        if (n) {
            d->setObject(key, n);
            n->release();
        }
    };
    set("WriteProbe_Writes", v->probeWrites);
    set("WriteProbe_Requested", v->probeRequested);
    set("WriteProbe_GetAddressRangeRC", v->probeRangeRC);
    set("WriteProbe_GetAddressRangeLength", v->probeRangeLength);
    set("WriteProbe_GetAddressRangeNonzeroBytes", v->probeRangeNonzero);
    set("WriteProbe_CreateMappingRC", v->probeMapRC);
    set("WriteProbe_CreateMappingLength", v->probeMapLength);
    set("WriteProbe_CreateMappingNonzeroBytes", v->probeMapNonzero);
    set("WriteProbe_ZeroPayloadWrites", v->probeZeroPayloadWrites);
    set("WriteProbe_TotalBytes", v->probeTotalBytes);
    set("WriteProbe_TotalNonzeroBytes", v->probeTotalNonzero);
    set("WriteProbe_RouteMismatches", v->probeMismatches);
    set("WriteProbe_RouteDiffBytes", v->probeRouteDiffBytes);

    // Parallel arrays, oldest first: one entry per recent write task.
    auto column = [&](const char * key, const uint64_t * values) {
        OSArray * a = OSArray::withCapacity(v->logCount);
        if (!a) {
            return;
        }
        uint32_t start = v->logCount < iSCSIKitDext_IVars::kProbeLog
            ? 0 : v->logIndex;
        for (uint32_t i = 0; i < v->logCount; i++) {
            OSNumber * n = OSNumber::withNumber(
                values[(start + i) % iSCSIKitDext_IVars::kProbeLog], 64);
            if (n) {
                a->setObject(n);
                n->release();
            }
        }
        d->setObject(key, a);
        a->release();
    };
    column("WriteProbe_Log_Opcode", v->logOpcode);
    column("WriteProbe_Log_LBA", v->logLBA);
    column("WriteProbe_Log_Length", v->logLength);
    column("WriteProbe_Log_RangeNonzero", v->logRangeNZ);
    column("WriteProbe_Log_MapNonzero", v->logMapNZ);
    column("WriteProbe_Log_RouteDiffBytes", v->logDiff);
    dext->SetProperties(d);
    d->release();
}

bool iSCSIKitDext::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(iSCSIKitDext_IVars, 1);
    if (!ivars) {
        return false;
    }
    ivars->lock = IOLockAlloc();
    if (!ivars->lock) {
        return false;
    }
    ivars->ring = reinterpret_cast<ISCSIKitEvent *>(
        IOMallocZero(sizeof(ISCSIKitEvent) * ISCSIKIT_EVENT_RING));
    LOG("init");
    return true;
}

void iSCSIKitDext::free()
{
    if (ivars) {
        OSSafeReleaseNULL(ivars->targetOpsQueue);
        if (ivars->lock) {
            IOLockFree(ivars->lock);
        }
        if (ivars->ring) {
            IOFree(ivars->ring, sizeof(ISCSIKitEvent) * ISCSIKIT_EVENT_RING);
            ivars->ring = nullptr;
        }
        IOSafeDeleteNULL(ivars, iSCSIKitDext_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(iSCSIKitDext, Start)
{
    // Queues must exist before the kernel connection is wired up in the
    // super Start. The framework dispatches UserCreateTargetForID (and
    // friends marked QUEUENAME(AuxiliaryQueue)) onto a controller queue
    // named "AuxiliaryQueue" that the dext is expected to create; without
    // it they fall back to the Default queue and starve the probe I/O
    // callbacks that need it.
    IODispatchQueue * auxiliary = nullptr;
    kern_return_t ret = IODispatchQueue::Create("AuxiliaryQueue", 0, 0, &auxiliary);
    if (ret != kIOReturnSuccess || !auxiliary) {
        LOG("auxiliary queue creation failed: 0x%x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = SetDispatchQueue("AuxiliaryQueue", auxiliary);
    auxiliary->release();
    if (ret != kIOReturnSuccess) {
        LOG("auxiliary queue set failed: 0x%x", ret);
        return ret;
    }
    ret = IODispatchQueue::Create("iSCSIKitTargetOps", 0, 0, &ivars->targetOpsQueue);
    if (ret != kIOReturnSuccess) {
        LOG("target-ops queue creation failed: 0x%x", ret);
        return ret;
    }

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    LOG("started");
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, Stop)
{
    LOG("stopping");
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(iSCSIKitDext, NewUserClient)
{
    (void)type;
    IOService * client = nullptr;
    kern_return_t ret = Create(this, "UserClientProperties", &client);
    if (ret != kIOReturnSuccess) {
        LOG("NewUserClient create failed: 0x%x", ret);
        return ret;
    }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        client->release();
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

#pragma mark - Daemon plumbing

bool iSCSIKitDext::DaemonSetUserClient(iSCSIKitUserClient * client)
{
    // Collect outstanding work under the lock; complete outside it. Kernel
    // RPCs must never run while holding the slot lock.
    OSAction * completions[kMaxTaskCount] = {};
    SCSIUserParallelResponse responses[kMaxTaskCount] = {};
    uint64_t seqs[kMaxTaskCount] = {};
    uint32_t failCount = 0;

    IOLockLock(ivars->lock);
    // Reject a second concurrent daemon: it must not silently take over the
    // targets an already-connected client is serving (a mix of "Connect All"
    // and the login agent, or two apps, would otherwise route a disk's I/O
    // to the wrong session).
    if (client && ivars->userClient && ivars->userClient != client) {
        IOLockUnlock(ivars->lock);
        return false;
    }
    ivars->userClient = client;
    if (!client) {
        for (auto & slot : ivars->slots) {
            if (slot.state != SlotState::free_) {
                SCSIUserParallelResponse response = {};
                response.version = kScsiUserParallelTaskResponseCurrentVersion1;
                response.fTargetID = slot.task.fTargetID;
                response.fControllerTaskIdentifier = slot.task.fControllerTaskIdentifier;
                response.fCompletionStatus = kSCSITaskStatus_DeviceNotPresent;
                response.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
                completions[failCount] = slot.completion;
                responses[failCount] = response;
                seqs[failCount] = slot.seq;
                recordEventLocked(ivars, kISCSIKitEvDisconnectFailed, slot.seq,
                                  slot.task.fControllerTaskIdentifier,
                                  static_cast<uint32_t>(slot.state), 0);
                failCount++;
                slot.completion = nullptr;
                OSSafeReleaseNULL(slot.map);
                OSSafeReleaseNULL(slot.buffer);
                if (slot.staged) {
                    IOFree(slot.staged, slot.stagedLength);
                    slot.staged = nullptr;
                    slot.stagedLength = 0;
                }
                slot.bufferAddress = 0;
                slot.bufferLength = 0;
                slot.bufferIOVA = 0;
                slot.state = SlotState::free_;
            }
        }
    }
    IOLockUnlock(ivars->lock);

    for (uint32_t i = 0; i < failCount; i++) {
        ParallelTaskCompletion(completions[i], responses[i]);
        recordEvent(ivars, kISCSIKitEvCompleted, seqs[i],
                    responses[i].fControllerTaskIdentifier,
                    responses[i].fCompletionStatus, 0, responses[i].fServiceResponse);
        OSSafeReleaseNULL(completions[i]);
    }
    return true;
}

kern_return_t iSCSIKitDext::DaemonRegisterTarget(uint64_t targetID)
{
    // Fresh counters per session, so one experiment cannot be read as another.
    IOLockLock(ivars->lock);
    ivars->probeWrites = 0;
    ivars->probeZeroPayloadWrites = 0;
    ivars->probeTotalBytes = 0;
    ivars->probeTotalNonzero = 0;
    ivars->probeMismatches = 0;
    ivars->probeRouteDiffBytes = 0;
    ivars->logIndex = 0;
    ivars->logCount = 0;
    IOLockUnlock(ivars->lock);

    if (!ivars->targetOpsQueue) {
        return kIOReturnNotReady;
    }
    // Fire and forget: blocking here would park the RPC dispatch queue that
    // the kernel needs for the probe I/O this call triggers. The daemon
    // observes success when the disk appears.
    retain();
    ivars->targetOpsQueue->DispatchAsync(^{
        OSDictionary * props = OSDictionary::withCapacity(1);
        if (props) {
            kern_return_t ret = UserCreateTargetForID(targetID, props);
            props->release();
            LOG("register target %llu: 0x%x", targetID, ret);
        }
        release();
    });
    return kIOReturnSuccess;
}

kern_return_t iSCSIKitDext::DaemonUnregisterTarget(uint64_t targetID)
{
    if (!ivars->targetOpsQueue) {
        return kIOReturnNotReady;
    }
    retain();
    ivars->targetOpsQueue->DispatchAsync(^{
        LOG("unregister target %llu", targetID);
        UserDestroyTargetForID(targetID);
        release();
    });
    return kIOReturnSuccess;
}

// Fails a task the daemon dequeued but could not be handed its descriptor:
// frees the slot and completes the task to the kernel with an error, so a
// failed dequeue never leaves the slot in flight forever. Matches by seq,
// never by task id, so it cannot touch a different task reusing the id.
static void failDequeuedTask(iSCSIKitDext * dext, iSCSIKitDext_IVars * v,
                             uint64_t seq, kern_return_t why)
{
    IOLockLock(v->lock);
    TaskSlot * found = nullptr;
    for (auto & slot : v->slots) {
        if (slot.state == SlotState::inFlight && slot.seq == seq) {
            found = &slot;
            break;
        }
    }
    if (!found) {
        IOLockUnlock(v->lock);
        return;
    }
    uint64_t taskID = found->task.fControllerTaskIdentifier;
    uint64_t targetID = found->task.fTargetID;
    OSAction * completion = found->completion;
    IOBufferMemoryDescriptor * buffer = found->buffer;
    IOMemoryMap * map = found->map;
    if (found->staged) {
        IOFree(found->staged, found->stagedLength);
    }
    found->staged = nullptr;
    found->stagedLength = 0;
    found->completion = nullptr;
    found->buffer = nullptr;
    found->map = nullptr;
    found->bufferAddress = 0;
    found->bufferLength = 0;
    found->bufferIOVA = 0;
    found->state = SlotState::free_;
    if (taskID >= 1 && taskID <= 64) {
        v->activeTaskIDs &= ~(1ULL << (taskID - 1));
    }
    recordEventLocked(v, kISCSIKitEvDequeueFailed, seq, taskID, static_cast<uint32_t>(why), 0);
    IOLockUnlock(v->lock);
    LOG("task %llu seq %llu: dequeue failed 0x%x; failing the task", taskID, seq, why);

    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(buffer);
    SCSIUserParallelResponse response = {};
    response.version = kScsiUserParallelTaskResponseCurrentVersion1;
    response.fTargetID = targetID;
    response.fControllerTaskIdentifier = taskID;
    response.fCompletionStatus = kSCSITaskStatus_No_Status;
    response.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    if (completion) {
        dext->ParallelTaskCompletion(completion, response);
        recordEvent(v, kISCSIKitEvCompleted, seq, taskID, response.fCompletionStatus, 0,
                    response.fServiceResponse);
        OSSafeReleaseNULL(completion);
    }
}

// Hands one pending task to the daemon. On failure after the slot went
// in flight, *seqOut is set so the caller can fail that exact task.
static kern_return_t dequeueTaskImpl(iSCSIKitDext_IVars * ivars, uint64_t taskID,
                                          IOUserClientMethodArguments * arguments,
                                          uint64_t * seqOut)
{
    IOLockLock(ivars->lock);
    TaskSlot * found = nullptr;
    for (auto & slot : ivars->slots) {
        if (slot.state == SlotState::pending &&
            slot.task.fControllerTaskIdentifier == taskID) {
            found = &slot;
            break;
        }
    }
    if (!found) {
        recordEventLocked(ivars, kISCSIKitEvDequeueNotFound, 0, taskID, kIOReturnNotFound, 0);
        IOLockUnlock(ivars->lock);
        return kIOReturnNotFound;
    }
    found->state = SlotState::inFlight;
    ivars->statDequeued++;
    SCSIUserParallelTask task = found->task;
    uint64_t seq = found->seq;
    *seqOut = seq;
    uint16_t firstNonzeroOffset = found->firstNonzeroOffset;
    // The slot is in flight: only this call, completion (which needs the
    // daemon to hold the descriptor) or a disconnect touch it now.
    const uint8_t * staged = found->staged;
    uint64_t stagedLength = found->stagedLength;
    IOLockUnlock(ivars->lock);

    ISCSIKitTaskDescriptor descriptor = {};
    descriptor.taskID = task.fControllerTaskIdentifier;
    descriptor.targetID = task.fTargetID;
    // SAM logical unit bytes, big-endian; single-level LUN lives in byte 1.
    descriptor.lun = task.fLogicalUnitBytes[1];
    descriptor.transferLength = static_cast<uint32_t>(task.fRequestedTransferCount);
    descriptor.direction = task.fTransferDirection;
    descriptor.cdbLength = task.fCommandSize;
    memcpy(descriptor.cdb, task.fCommandDescriptorBlock, sizeof(descriptor.cdb));
    memcpy(descriptor.reserved, &firstNonzeroOffset, sizeof(uint16_t));
    descriptor.seq = seq;

    uint64_t payloadLength = 0;
    if (task.fTransferDirection == kISCSIKitWrite) {
        payloadLength = task.fRequestedTransferCount;
    }

    uint64_t totalLength = sizeof(descriptor) + payloadLength;
    uint8_t * bytes = reinterpret_cast<uint8_t *>(IOMallocZero(totalLength));
    if (!bytes) {
        return kIOReturnNoMemory;
    }
    memcpy(bytes, &descriptor, sizeof(descriptor));

    if (payloadLength > 0) {
        // A write must reach the daemon whole or not at all. Failing here
        // fails the task in DaemonDequeueTask.
        if (staged == nullptr || stagedLength < payloadLength) {
            IOFree(bytes, totalLength);
            return kIOReturnUnderrun;
        }
        memcpy(bytes + sizeof(descriptor), staged, payloadLength);
    }

    // Large outputs arrive as an IOMemoryDescriptor to fill; small ones as
    // an OSData we create. Handle both or DequeueTask fails for big buffers.
    if (arguments->structureOutputDescriptor) {
        IOMemoryMap * map = nullptr;
        kern_return_t ret = arguments->structureOutputDescriptor->CreateMapping(
            0, 0, 0, 0, 0, &map);
        if (ret != kIOReturnSuccess || !map) {
            IOFree(bytes, totalLength);
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        uint64_t mapLength = map->GetLength();
        if (mapLength < totalLength) {
            map->release();
            IOFree(bytes, totalLength);
            return kIOReturnNoSpace;
        }
        memcpy(reinterpret_cast<void *>(map->GetAddress()), bytes, totalLength);
        map->release();
        IOFree(bytes, totalLength);
        recordEvent(ivars, kISCSIKitEvDequeueOK, seq, taskID, 0, totalLength);
        return kIOReturnSuccess;
    }
    arguments->structureOutput = OSData::withBytes(bytes, totalLength);
    IOFree(bytes, totalLength);
    if (!arguments->structureOutput) {
        return kIOReturnNoMemory;
    }
    recordEvent(ivars, kISCSIKitEvDequeueOK, seq, taskID, 0, totalLength);
    return kIOReturnSuccess;
}

kern_return_t iSCSIKitDext::DaemonDequeueTask(uint64_t taskID,
                                              IOUserClientMethodArguments * arguments)
{
    uint64_t seq = 0;
    kern_return_t ret = dequeueTaskImpl(ivars, taskID, arguments, &seq);
    if (ret != kIOReturnSuccess && seq != 0) {
        // The slot already went in flight: complete the task with an error
        // here, or nothing ever would (the daemon has no descriptor).
        failDequeuedTask(this, ivars, seq, ret);
    }
    return ret;
}

kern_return_t iSCSIKitDext::DaemonListPending(uint64_t minAgeMs,
                                              IOUserClientMethodArguments * arguments)
{
    // Triples of (taskID, ageMs, seq) for every slot still `pending` (queued, never
    // dequeued) for at least minAgeMs: a task whose notification was lost.
    // entries[0] = count, then count pairs. Fixed size, so the output is
    // never empty (an empty OSData is not guaranteed to be creatable).
    uint64_t entries[1 + 3 * kMaxTaskCount] = {};
    uint32_t count = 0;
    uint64_t now = nowNs();
    IOLockLock(ivars->lock);
    for (auto & slot : ivars->slots) {
        if (slot.state != SlotState::pending) {
            continue;
        }
        uint64_t ageMs = now > slot.queuedAtNs ? (now - slot.queuedAtNs) / 1000000 : 0;
        if (ageMs >= minAgeMs) {
            entries[1 + 3 * count] = slot.task.fControllerTaskIdentifier;
            entries[2 + 3 * count] = ageMs;
            entries[3 + 3 * count] = slot.seq;
            count++;
        }
    }
    ivars->statListPendingReported += count;
    IOLockUnlock(ivars->lock);
    if (count > 0) {
        LOG("%u task(s) pending without dequeue for >= %llu ms", count, minAgeMs);
    }

    // 1 Hz from the daemon: a cheap place to keep the registry counters fresh.
    publishTaskStats(this, ivars);

    entries[0] = count;
    uint64_t totalLength = sizeof(entries);
    if (arguments->structureOutputDescriptor) {
        IOMemoryMap * map = nullptr;
        kern_return_t ret = arguments->structureOutputDescriptor->CreateMapping(
            0, 0, 0, 0, 0, &map);
        if (ret != kIOReturnSuccess || !map) {
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        if (map->GetLength() < totalLength) {
            map->release();
            return kIOReturnNoSpace;
        }
        memcpy(reinterpret_cast<void *>(map->GetAddress()), entries, totalLength);
        map->release();
        return kIOReturnSuccess;
    }
    arguments->structureOutput = OSData::withBytes(entries, totalLength);
    return arguments->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

kern_return_t iSCSIKitDext::DaemonReadEvents(uint64_t startIndex,
                                             IOUserClientMethodArguments * arguments)
{
    if (!ivars->ring) {
        return kIOReturnNoMemory;
    }
    const uint64_t maxBytes = sizeof(ISCSIKitEventBatchHeader) +
        sizeof(ISCSIKitEvent) * ISCSIKIT_EVENT_BATCH;
    uint8_t * out = reinterpret_cast<uint8_t *>(IOMallocZero(maxBytes));
    if (!out) {
        return kIOReturnNoMemory;
    }
    ISCSIKitEventBatchHeader header = {};
    ISCSIKitEvent * events = reinterpret_cast<ISCSIKitEvent *>(out + sizeof(header));
    IOLockLock(ivars->lock);
    uint64_t oldest = ivars->ringNext > ISCSIKIT_EVENT_RING
        ? ivars->ringNext - ISCSIKIT_EVENT_RING : 0;
    uint64_t first = startIndex < oldest ? oldest : startIndex;
    if (first > ivars->ringNext) {
        first = ivars->ringNext;  // reader ahead (dext restarted): resync
    }
    uint64_t count = ivars->ringNext - first;
    if (count > ISCSIKIT_EVENT_BATCH) {
        count = ISCSIKIT_EVENT_BATCH;
    }
    for (uint64_t i = 0; i < count; i++) {
        events[i] = ivars->ring[(first + i) % ISCSIKIT_EVENT_RING];
    }
    IOLockUnlock(ivars->lock);
    header.firstIndex = first;
    header.count = count;
    header.nextIndex = first + count;
    memcpy(out, &header, sizeof(header));

    // Fixed size output (the daemon passes a buffer of maxBytes); unused
    // tail is zero.
    kern_return_t ret = kIOReturnSuccess;
    if (arguments->structureOutputDescriptor) {
        IOMemoryMap * map = nullptr;
        ret = arguments->structureOutputDescriptor->CreateMapping(0, 0, 0, 0, 0, &map);
        if (ret == kIOReturnSuccess && map) {
            if (map->GetLength() >= maxBytes) {
                memcpy(reinterpret_cast<void *>(map->GetAddress()), out, maxBytes);
            } else {
                ret = kIOReturnNoSpace;
            }
            map->release();
        } else if (ret == kIOReturnSuccess) {
            ret = kIOReturnNoMemory;
        }
    } else {
        arguments->structureOutput = OSData::withBytes(out, maxBytes);
        ret = arguments->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    IOFree(out, maxBytes);
    return ret;
}

kern_return_t iSCSIKitDext::DaemonCompleteTask(const void * bytes, uint64_t length)
{
    if (length < sizeof(ISCSIKitTaskResponse)) {
        return kIOReturnBadArgument;
    }
    ISCSIKitTaskResponse reply = {};
    memcpy(&reply, bytes, sizeof(reply));

    IOLockLock(ivars->lock);
    TaskSlot * found = nullptr;
    for (auto & slot : ivars->slots) {
        if (slot.state == SlotState::inFlight &&
            slot.task.fControllerTaskIdentifier == reply.taskID) {
            found = &slot;
            break;
        }
    }
    if (!found) {
        recordEventLocked(ivars, kISCSIKitEvResponseNotFound, 0, reply.taskID, reply.status,
                          reply.bytesTransferred);
        IOLockUnlock(ivars->lock);
        return kIOReturnNotFound;
    }
    uint64_t seq = found->seq;
    recordEventLocked(ivars, kISCSIKitEvResponse, seq, reply.taskID, reply.status,
                      reply.bytesTransferred);
    SCSIUserParallelTask task = found->task;
    OSAction * completion = found->completion;
    IOBufferMemoryDescriptor * buffer = found->buffer;
    IOMemoryMap * map = found->map;
    uint64_t bufferAddress = found->bufferAddress;
    uint64_t bufferLength = found->bufferLength;
    if (found->staged) {
        IOFree(found->staged, found->stagedLength);
        found->staged = nullptr;
        found->stagedLength = 0;
    }
    if (reply.taskID >= 1 && reply.taskID <= 64) {
        ivars->activeTaskIDs &= ~(1ULL << (reply.taskID - 1));
    }
    found->completion = nullptr;
    found->buffer = nullptr;
    found->map = nullptr;
    found->bufferAddress = 0;
    found->bufferLength = 0;
    found->state = SlotState::free_;
    bool publishStats = (++ivars->statCompleted % 32) == 0;
    IOLockUnlock(ivars->lock);
    if (publishStats) {
        publishTaskStats(this, ivars);
    }

    // Bytes the kernel may treat as transferred. For reads this is what was
    // actually copied into its buffer, never what the daemon announced; a
    // read that could not be delivered is reported as a failure.
    uint64_t requested = task.fRequestedTransferCount;
    uint64_t delivered = reply.bytesTransferred < requested ? reply.bytesTransferred : requested;
    bool deliveryFailed = false;
    if (task.fTransferDirection == kISCSIKitRead) {
        delivered = 0;
        if (reply.bytesTransferred > 0 && bufferAddress == 0) {
            deliveryFailed = true;
        }
    }

    // Read data comes back inline after the response header; copy it into
    // the task's buffer mapped when the task was queued.
    if (task.fTransferDirection == kISCSIKitRead && reply.bytesTransferred > 0 &&
        bufferAddress != 0) {
        uint64_t available = length - sizeof(ISCSIKitTaskResponse);
        uint64_t dataLength = reply.bytesTransferred < available ? reply.bytesTransferred : available;
        uint64_t copyLength = dataLength < bufferLength ? dataLength : bufferLength;
        if (copyLength < dataLength) {
            LOG("task %llu: read data %llu bytes truncated to buffer %llu",
                reply.taskID, dataLength, bufferLength);
        }
        if (copyLength > requested) {
            copyLength = requested;
        }
        memcpy(reinterpret_cast<void *>(bufferAddress),
               reinterpret_cast<const uint8_t *>(bytes) + sizeof(ISCSIKitTaskResponse),
               copyLength);
        delivered = copyLength;
        if (copyLength < reply.bytesTransferred && copyLength < requested) {
            deliveryFailed = true;
        }
    }
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(buffer);

    SCSIUserParallelResponse response = {};
    response.version = kScsiUserParallelTaskResponseCurrentVersion1;
    response.fTargetID = task.fTargetID;
    response.fControllerTaskIdentifier = reply.taskID;
    response.fCompletionStatus = static_cast<SCSITaskStatus>(reply.status);
    response.fServiceResponse = deliveryFailed
        ? kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE
        : kSCSIServiceResponse_TASK_COMPLETE;
    // Never more than requested nor more than delivered: the count is passed
    // straight up to the syscall.
    response.fBytesTransferred = deliveryFailed ? 0 : delivered;
    // senseLength is uint8_t, so it always fits kMaxSenseBufferSize (256).
    response.fSenseLength = reply.senseLength;
    memcpy(response.fSenseBuffer, reply.sense, reply.senseLength);

    ParallelTaskCompletion(completion, response);
    recordEvent(ivars, kISCSIKitEvCompleted, seq, reply.taskID, response.fCompletionStatus,
                response.fBytesTransferred, response.fServiceResponse);
    OSSafeReleaseNULL(completion);
    return kIOReturnSuccess;
}

#pragma mark - Completion type definitions

void IMPL(iSCSIKitDext, ParallelTaskCompletion)
{
    // Completions flow dext -> kernel through UserCompleteParallelTask;
    // this target method is never invoked on the dext.
    (void)action;
    (void)response;
}

void IMPL(iSCSIKitDext, BundledParallelTaskCompletion)
{
    (void)action;
    (void)parallelResponseSlotIndices;
    (void)parallelResponseSlotIndicesCount;
}

#pragma mark - HBA characterization

kern_return_t IMPL(iSCSIKitDext, UserReportHBAHighestLogicalUnitNumber)
{
    *value = 0;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserDoesHBASupportSCSIParallelFeature)
{
    (void)theValue;
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserInitializeTargetForID)
{
    LOG("initialize target %llu", targetID);
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserDoesHBAPerformAutoSense)
{
    *result = true;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserDoesHBASupportMultiPathing)
{
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserReportInitiatorIdentifier)
{
    *id = 7;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserReportHighestSupportedDeviceID)
{
    *id = 15;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserReportMaximumTaskCount)
{
    *count = kMaxTaskCount;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserDoesHBAPerformDeviceManagement)
{
    // Targets appear and disappear with iSCSI sessions; we manage them.
    *result = true;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserGetDMASpecification)
{
    *maxTransferSize = kMaxTransferSize;
    *alignment = 4;
    *numAddressBits = 64;
    *segmentType = kDMAOutputSegmentHost64;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserMapHBAData)
{
    static uint32_t nextTaskID = 1;
    *uniqueTaskID = nextTaskID++;
    return kIOReturnSuccess;
}

#pragma mark - Controller lifecycle

kern_return_t IMPL(iSCSIKitDext, UserInitializeController)
{
    LOG("initialize controller");

    OSDictionary * constraints = OSDictionary::withCapacity(8);
    if (!constraints) {
        return kIOReturnNoMemory;
    }

    auto setNumber = [&](const char * key, uint64_t value) {
        OSNumber * number = OSNumber::withNumber(value, 64);
        if (number) {
            constraints->setObject(key, number);
            number->release();
        }
    };

    // SCSIUserParallelTask carries a single fBufferIOVMAddr and no
    // scatter-gather list, and a virtual controller has no DART to make a
    // multi-page user buffer IOVM-contiguous. Allowing more than one segment
    // made every request larger than one page fail with EIO before it ever
    // reached the dext (build 25/26). So a task is bounded by one physically
    // contiguous page: 16 KiB on Apple Silicon. Throughput has to come from
    // queue depth (kMaxTaskCount tasks in flight), not from bigger tasks.
    setNumber(kIOMaximumSegmentCountReadKey, 1);
    setNumber(kIOMaximumSegmentCountWriteKey, 1);
    setNumber(kIOMaximumSegmentByteCountReadKey, kMaxTaskBytes);
    setNumber(kIOMaximumSegmentByteCountWriteKey, kMaxTaskBytes);
    setNumber(kIOMinimumSegmentAlignmentByteCountKey, 4);
    setNumber(kIOMaximumSegmentAddressableBitCountKey, 64);
    setNumber(kIOMinimumHBADataAlignmentMaskKey, 0xFFFFFFFFFFFFFFFF);

    kern_return_t ret = UserReportHBAConstraints(constraints);
    constraints->release();
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // The per-command transfer ceiling is NOT taken from the constraints
    // dictionary: the SDK states UserReportHBAConstraints ignores
    // kIOMaximumByteCount{Read,Write}Key and that they must be set as
    // properties on the dext. Without them the kernel keeps its default
    // (16 KiB) regardless of the segment byte count, which is exactly the
    // flat ~2 MiB/s at every I/O size measured with kMaxTransferSize = 1 MiB.
    OSDictionary * limits = OSDictionary::withCapacity(2);
    if (!limits) {
        return kIOReturnNoMemory;
    }
    auto setLimit = [&](const char * key, uint64_t value) {
        OSNumber * number = OSNumber::withNumber(value, 64);
        if (number) {
            limits->setObject(key, number);
            number->release();
        }
    };
    setLimit(kIOMaximumByteCountReadKey, kMaxTaskBytes);
    setLimit(kIOMaximumByteCountWriteKey, kMaxTaskBytes);
    ret = SetProperties(limits);
    limits->release();
    if (ret != kIOReturnSuccess) {
        LOG("SetProperties(max byte count) failed: 0x%x", ret);
    }
    return ret;
}

kern_return_t IMPL(iSCSIKitDext, UserStartController)
{
    LOG("start controller");
    return kIOReturnSuccess;
}

#pragma mark - Task processing

kern_return_t IMPL(iSCSIKitDext, UserProcessParallelTask)
{
    // Fetch the data buffer here: UserGetDataBuffer is only legal inside
    // UserProcessParallelTask. The slot keeps it until completion.
    const uint64_t taskID = parallelRequest.fControllerTaskIdentifier;
    IOLockLock(ivars->lock);
    const uint64_t seq = ++ivars->nextSeq;
    recordEventLocked(ivars, kISCSIKitEvReceived, seq, taskID,
                      parallelRequest.fTransferDirection,
                      parallelRequest.fRequestedTransferCount);
    IOLockUnlock(ivars->lock);
    IOBufferMemoryDescriptor * buffer = nullptr;
    IOAddressSegment range = {};
    IOMemoryMap * map = nullptr;
    uint64_t mapAddress = 0, mapLength = 0;
    kern_return_t rangeRC = kIOReturnError, mapRC = kIOReturnError;
    if (parallelRequest.fTransferDirection != kISCSIKitNoData &&
        parallelRequest.fRequestedTransferCount > 0) {
        kern_return_t ret = UserGetDataBuffer(parallelRequest.fTargetID,
                                              parallelRequest.fControllerTaskIdentifier,
                                              &buffer);
        if (ret != kIOReturnSuccess || !buffer) {
            LOG("task %llu: UserGetDataBuffer failed 0x%x (dir %u, %llu bytes requested)",
                parallelRequest.fControllerTaskIdentifier, ret,
                parallelRequest.fTransferDirection,
                parallelRequest.fRequestedTransferCount);
            recordEvent(ivars, kISCSIKitEvBufferFailed, seq, taskID,
                        static_cast<uint32_t>(ret != kIOReturnSuccess ? ret : kIOReturnNoMemory), 0);
            *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        // GetAddressRange is LOCALONLY: it reports the local state of a
        // descriptor the dext created itself, so for a descriptor handed over
        // by the kernel it can fail or report nothing useful. Keep its result
        // for comparison, but reach the bytes through a real mapping.
        rangeRC = buffer->GetAddressRange(&range);
        mapRC = buffer->CreateMapping(0, 0, 0, 0, 0, &map);
        if (mapRC == kIOReturnSuccess && map) {
            mapAddress = map->GetAddress();
            mapLength = map->GetLength();
        }
        if (mapAddress == 0 && range.address == 0) {
            LOG("task %llu: no way to reach the data buffer (range 0x%x, map 0x%x)",
                parallelRequest.fControllerTaskIdentifier, rangeRC, mapRC);
        }
    }

    IOLockLock(ivars->lock);
    if (!ivars->userClient) {
        recordEventLocked(ivars, kISCSIKitEvNoClient, seq, taskID, kIOReturnNotReady, 0);
        IOLockUnlock(ivars->lock);
        OSSafeReleaseNULL(map);
        OSSafeReleaseNULL(buffer);
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return kIOReturnNotReady;
    }
    TaskSlot * slot = nullptr;
    for (auto & candidate : ivars->slots) {
        if (candidate.state == SlotState::free_) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        recordEventLocked(ivars, kISCSIKitEvNoSlot, seq, taskID, kIOReturnNoResources, 0);
        IOLockUnlock(ivars->lock);
        OSSafeReleaseNULL(map);
        OSSafeReleaseNULL(buffer);
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return kIOReturnNoResources;
    }
    slot->state = SlotState::pending;
    slot->queuedAtNs = nowNs();
    slot->seq = seq;
    slot->task = parallelRequest;
    completion->retain();
    slot->completion = completion;
    slot->buffer = buffer;
    slot->map = map;
    // Prefer the address that is known to work for the inbound copy, and fall
    // back to the mapping when GetAddressRange reported nothing usable.
    slot->bufferAddress = range.address != 0 ? range.address : mapAddress;
    slot->bufferLength = range.address != 0 ? range.length : mapLength;
    slot->bufferIOVA = parallelRequest.fBufferIOVMAddr;
    // Duplicate-task-ID detection: Apple DTS diagnoses zeroed write buffers
    // as wrong-task lookups caused by reused controller task IDs.
    uint64_t taskBit = (parallelRequest.fControllerTaskIdentifier >= 1 &&
                        parallelRequest.fControllerTaskIdentifier <= 64)
        ? (1ULL << (parallelRequest.fControllerTaskIdentifier - 1)) : 0;
    bool duplicateID = taskBit != 0 && (ivars->activeTaskIDs & taskBit) != 0;
    ivars->activeTaskIDs |= taskBit;

    // Capture the write payload HERE, inside the documented context for
    // UserGetDataBuffer, and never touch the buffer again afterwards.
    slot->staged = nullptr;
    slot->stagedLength = 0;
    slot->firstNonzeroOffset = duplicateID ? 0xDEAD : 0x0;
    if (parallelRequest.fTransferDirection == kISCSIKitWrite &&
        parallelRequest.fRequestedTransferCount > 0) {
        uint64_t stageLength = parallelRequest.fRequestedTransferCount;
        // Count the payload through both routes before deciding, and publish
        // the comparison: this is what tells us whether the kernel stages the
        // outbound data somewhere GetAddressRange cannot see.
        uint64_t rangeNonzero = 0, mapNonzero = 0;
        if (range.address != 0 && stageLength <= range.length) {
            const uint8_t * p = reinterpret_cast<const uint8_t *>(range.address);
            for (uint64_t i = 0; i < stageLength; i++) {
                rangeNonzero += p[i] != 0;
            }
        }
        if (mapAddress != 0 && stageLength <= mapLength) {
            const uint8_t * p = reinterpret_cast<const uint8_t *>(mapAddress);
            for (uint64_t i = 0; i < stageLength; i++) {
                mapNonzero += p[i] != 0;
            }
        }
        // The counts above can match while the bytes differ, so compare the
        // two routes byte by byte before claiming they agree.
        uint64_t routeDiff = 0;
        bool routesComparable = range.address != 0 && mapAddress != 0 &&
            stageLength <= range.length && stageLength <= mapLength;
        if (routesComparable) {
            const uint8_t * a = reinterpret_cast<const uint8_t *>(range.address);
            const uint8_t * b = reinterpret_cast<const uint8_t *>(mapAddress);
            for (uint64_t i = 0; i < stageLength; i++) {
                routeDiff += a[i] != b[i];
            }
        }
        ivars->probeRouteDiffBytes += routeDiff;

        // WRITE(10)/(12) carry a 4-byte LBA, WRITE(16) an 8-byte one.
        uint64_t lba = 0;
        const uint8_t * cdbBytes = parallelRequest.fCommandDescriptorBlock;
        uint32_t lbaWidth = cdbBytes[0] == 0x8A ? 8 : 4;
        for (uint32_t i = 0; i < lbaWidth; i++) {
            lba = (lba << 8) | cdbBytes[2 + i];
        }
        uint32_t slotIndex = ivars->logIndex;
        ivars->logOpcode[slotIndex] = cdbBytes[0];
        ivars->logLBA[slotIndex] = lba;
        ivars->logLength[slotIndex] = stageLength;
        ivars->logRangeNZ[slotIndex] = rangeNonzero;
        ivars->logMapNZ[slotIndex] = mapNonzero;
        ivars->logDiff[slotIndex] = routesComparable ? routeDiff : 0xFFFFFFFF;
        ivars->logIndex = (slotIndex + 1) % iSCSIKitDext_IVars::kProbeLog;
        if (ivars->logCount < iSCSIKitDext_IVars::kProbeLog) {
            ivars->logCount++;
        }

        ivars->probeWrites++;
        ivars->probeRequested = stageLength;
        ivars->probeRangeRC = static_cast<uint32_t>(rangeRC);
        ivars->probeRangeLength = range.length;
        ivars->probeRangeNonzero = rangeNonzero;
        ivars->probeMapRC = static_cast<uint32_t>(mapRC);
        ivars->probeMapLength = mapLength;
        ivars->probeMapNonzero = mapNonzero;
        ivars->probeTotalBytes += stageLength;
        ivars->probeTotalNonzero += mapNonzero > rangeNonzero ? mapNonzero : rangeNonzero;
        if (rangeNonzero == 0 && mapNonzero == 0) {
            ivars->probeZeroPayloadWrites++;
        }
        if (rangeNonzero != mapNonzero) {
            ivars->probeMismatches++;
        }

        // Stage from the route that actually carries the data; the mapping is
        // the documented one, so it wins when both are usable.
        uint64_t sourceAddress = 0, sourceLength = 0;
        if (mapAddress != 0 && stageLength <= mapLength && (mapNonzero > 0 || rangeNonzero == 0)) {
            sourceAddress = mapAddress;
            sourceLength = mapLength;
        } else if (range.address != 0) {
            sourceAddress = range.address;
            sourceLength = range.length;
        }
        // A write whose payload cannot be fully captured must FAIL, never be
        // silently sent as zeros to the target.
        if (sourceAddress == 0 || stageLength > sourceLength) {
            slot->state = SlotState::free_;
            slot->completion = nullptr;
            OSSafeReleaseNULL(completion);
            ivars->activeTaskIDs &= ~taskBit;
            slot->map = nullptr;
            slot->buffer = nullptr;
            recordEventLocked(ivars, kISCSIKitEvBufferFailed, seq, taskID, kIOReturnNoMemory, 0);
            IOLockUnlock(ivars->lock);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(buffer);
            *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            return kIOReturnNoMemory;
        }
        uint8_t * staged = reinterpret_cast<uint8_t *>(IOMallocZero(stageLength));
        if (!staged) {
            slot->state = SlotState::free_;
            slot->completion = nullptr;
            OSSafeReleaseNULL(completion);
            ivars->activeTaskIDs &= ~taskBit;
            slot->map = nullptr;
            slot->buffer = nullptr;
            recordEventLocked(ivars, kISCSIKitEvBufferFailed, seq, taskID, kIOReturnNoMemory, 0);
            IOLockUnlock(ivars->lock);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(buffer);
            *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            return kIOReturnNoMemory;
        }
        memcpy(staged, reinterpret_cast<const void *>(sourceAddress), stageLength);
        slot->staged = staged;
        slot->stagedLength = stageLength;
        for (uint64_t i = 0; i < stageLength; i++) {
            if (staged[i] != 0) {
                slot->firstNonzeroOffset = 0x1;
                break;
            }
        }
    }
    iSCSIKitUserClient * client = ivars->userClient;
    uint64_t pendingNow = 0;
    for (auto & candidate : ivars->slots) {
        pendingNow += candidate.state == SlotState::pending;
    }
    if (pendingNow > ivars->statMaxPending) {
        ivars->statMaxPending = pendingNow;
    }
    bool publishStats = (++ivars->statReceived % 32) == 0;
    recordEventLocked(ivars, kISCSIKitEvSlotReserved, seq, taskID, 0, pendingNow);
    bool isWrite = parallelRequest.fTransferDirection == kISCSIKitWrite &&
                   parallelRequest.fRequestedTransferCount > 0;
    IOLockUnlock(ivars->lock);

    if (isWrite) {
        publishWriteProbe(this, ivars);
    }
    bool notified = client->NotifyTaskPending(taskID, seq);
    // Fail the task here when no notification went out: otherwise it sits
    // pending until something else notices it.
    OSAction * orphanCompletion = nullptr;
    IOBufferMemoryDescriptor * orphanBuffer = nullptr;
    IOMemoryMap * orphanMap = nullptr;
    IOLockLock(ivars->lock);
    if (notified) {
        ivars->statNotified++;
        recordEventLocked(ivars, kISCSIKitEvNotifyOK, seq, taskID, 0, 0);
    } else {
        ivars->statNotifyFailed++;
        publishStats = true;
        recordEventLocked(ivars, kISCSIKitEvNotifyNoCallback, seq, taskID, kIOReturnNotReady, 0);
        // Only if still pending: the stuck sweep may have dequeued it.
        if (slot->state == SlotState::pending && slot->seq == seq) {
            orphanCompletion = slot->completion;
            orphanBuffer = slot->buffer;
            orphanMap = slot->map;
            if (slot->staged) {
                IOFree(slot->staged, slot->stagedLength);
            }
            slot->staged = nullptr;
            slot->stagedLength = 0;
            slot->completion = nullptr;
            slot->buffer = nullptr;
            slot->map = nullptr;
            slot->bufferAddress = 0;
            slot->bufferLength = 0;
            slot->bufferIOVA = 0;
            slot->state = SlotState::free_;
            ivars->activeTaskIDs &= ~taskBit;
        }
    }
    IOLockUnlock(ivars->lock);
    if (publishStats) {
        publishTaskStats(this, ivars);
    }
    if (!notified && orphanCompletion) {
        OSSafeReleaseNULL(orphanMap);
        OSSafeReleaseNULL(orphanBuffer);
        OSSafeReleaseNULL(orphanCompletion);
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return kIOReturnNotReady;
    }
    *response = kSCSIServiceResponse_Request_In_Process;
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitDext, UserMapBundledParallelTaskCommandAndResponseBuffers)
{
    // Decline shared buffers; use the single-task path for now.
    (void)parallelCommandIOMemoryDescriptor;
    (void)parallelResponseIOMemoryDescriptor;
    return kIOReturnUnsupported;
}

void IMPL(iSCSIKitDext, UserProcessBundledParallelTasks)
{
    (void)parallelRequestSlotIndices;
    (void)parallelRequestSlotIndicesCount;
    (void)completion;
}

#pragma mark - SAM-2 task management

kern_return_t IMPL(iSCSIKitDext, UserAbortTaskRequest)
{
    (void)theT; (void)theL; (void)theQ;
    *response = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(iSCSIKitDext, UserAbortTaskSetRequest)
{
    (void)theT; (void)theL;
    *response = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(iSCSIKitDext, UserClearACARequest)
{
    (void)theT; (void)theL;
    *response = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(iSCSIKitDext, UserClearTaskSetRequest)
{
    (void)theT; (void)theL;
    *response = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(iSCSIKitDext, UserLogicalUnitResetRequest)
{
    (void)theT; (void)theL;
    *response = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(iSCSIKitDext, UserTargetResetRequest)
{
    (void)theT;
    *response = 0;
    return kIOReturnUnsupported;
}

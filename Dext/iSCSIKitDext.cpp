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
#include <SCSIControllerDriverKit/IOSCSIParallelControllerCharacteristics.h>
#include "iSCSIKitDext.h"
#include "iSCSIKitUserClient.h"
#include "iSCSIKitProtocol.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "iSCSIKitDext: " fmt, ##__VA_ARGS__)

static constexpr uint32_t kMaxTaskCount = 64;
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
};

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
};

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

kern_return_t iSCSIKitDext::DaemonDequeueTask(uint64_t taskID,
                                              IOUserClientMethodArguments * arguments)
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
        IOLockUnlock(ivars->lock);
        return kIOReturnNotFound;
    }
    found->state = SlotState::inFlight;
    SCSIUserParallelTask task = found->task;
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
    memcpy(descriptor.reserved, &found->firstNonzeroOffset, sizeof(uint16_t));

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

    if (payloadLength > 0 && found->staged != nullptr) {
        uint64_t copyLength = payloadLength < found->stagedLength
            ? payloadLength : found->stagedLength;
        memcpy(bytes + sizeof(descriptor), found->staged, copyLength);
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
        uint64_t copyLength = totalLength < mapLength ? totalLength : mapLength;
        memcpy(reinterpret_cast<void *>(map->GetAddress()), bytes, copyLength);
        map->release();
        IOFree(bytes, totalLength);
        return kIOReturnSuccess;
    }
    arguments->structureOutput = OSData::withBytes(bytes, totalLength);
    IOFree(bytes, totalLength);
    return arguments->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
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
        IOLockUnlock(ivars->lock);
        return kIOReturnNotFound;
    }
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
    IOLockUnlock(ivars->lock);

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
        memcpy(reinterpret_cast<void *>(bufferAddress),
               reinterpret_cast<const uint8_t *>(bytes) + sizeof(ISCSIKitTaskResponse),
               copyLength);
    }
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(buffer);

    SCSIUserParallelResponse response = {};
    response.version = kScsiUserParallelTaskResponseCurrentVersion1;
    response.fTargetID = task.fTargetID;
    response.fControllerTaskIdentifier = reply.taskID;
    response.fCompletionStatus = static_cast<SCSITaskStatus>(reply.status);
    response.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    response.fBytesTransferred = reply.bytesTransferred;
    // senseLength is uint8_t, so it always fits kMaxSenseBufferSize (256).
    response.fSenseLength = reply.senseLength;
    memcpy(response.fSenseBuffer, reply.sense, reply.senseLength);

    ParallelTaskCompletion(completion, response);
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
        IOLockUnlock(ivars->lock);
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
        IOLockUnlock(ivars->lock);
        OSSafeReleaseNULL(buffer);
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return kIOReturnNoResources;
    }
    slot->state = SlotState::pending;
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
    bool isWrite = parallelRequest.fTransferDirection == kISCSIKitWrite &&
                   parallelRequest.fRequestedTransferCount > 0;
    IOLockUnlock(ivars->lock);

    if (isWrite) {
        publishWriteProbe(this, ivars);
    }
    client->NotifyTaskPending(parallelRequest.fControllerTaskIdentifier);
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

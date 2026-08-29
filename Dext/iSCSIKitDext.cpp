// iSCSIKit virtual HBA. Queues SCSI tasks from the kernel and hands them to
// iscsikitd through iSCSIKitUserClient; the daemon executes them over iSCSI
// and completes them back here.

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSData.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOUserClient.h>
#include <SCSIControllerDriverKit/IOSCSIParallelControllerCharacteristics.h>
#include "iSCSIKitDext.h"
#include "iSCSIKitUserClient.h"
#include "iSCSIKitProtocol.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "iSCSIKitDext: " fmt, ##__VA_ARGS__)

static constexpr uint32_t kMaxTaskCount = 64;
static constexpr uint64_t kMaxTransferSize = ISCSIKIT_MAX_TRANSFER;

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
};

}  // namespace

struct iSCSIKitDext_IVars {
    IOLock * lock;
    iSCSIKitUserClient * userClient;  // not retained; cleared on client Stop
    TaskSlot slots[kMaxTaskCount];
};

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
        if (ivars->lock) {
            IOLockFree(ivars->lock);
        }
        IOSafeDeleteNULL(ivars, iSCSIKitDext_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(iSCSIKitDext, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
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

void iSCSIKitDext::DaemonSetUserClient(iSCSIKitUserClient * client)
{
    // Collect outstanding work under the lock; complete outside it. Kernel
    // RPCs must never run while holding the slot lock.
    OSAction * completions[kMaxTaskCount] = {};
    SCSIUserParallelResponse responses[kMaxTaskCount] = {};
    uint32_t failCount = 0;

    IOLockLock(ivars->lock);
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
                OSSafeReleaseNULL(slot.buffer);
                slot.bufferAddress = 0;
                slot.bufferLength = 0;
                slot.state = SlotState::free_;
            }
        }
    }
    IOLockUnlock(ivars->lock);

    for (uint32_t i = 0; i < failCount; i++) {
        ParallelTaskCompletion(completions[i], responses[i]);
        OSSafeReleaseNULL(completions[i]);
    }
}

kern_return_t iSCSIKitDext::DaemonRegisterTarget(uint64_t targetID)
{
    OSDictionary * props = OSDictionary::withCapacity(1);
    if (!props) {
        return kIOReturnNoMemory;
    }
    kern_return_t ret = UserCreateTargetForID(targetID, props);
    props->release();
    LOG("register target %llu: 0x%x", targetID, ret);
    return ret;
}

kern_return_t iSCSIKitDext::DaemonUnregisterTarget(uint64_t targetID)
{
    LOG("unregister target %llu", targetID);
    return UserDestroyTargetForID(targetID);
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

    if (payloadLength > 0 && found->bufferAddress != 0) {
        uint64_t copyLength = payloadLength < found->bufferLength
            ? payloadLength : found->bufferLength;
        memcpy(bytes + sizeof(descriptor),
               reinterpret_cast<const void *>(found->bufferAddress),
               copyLength);
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
    uint64_t bufferAddress = found->bufferAddress;
    uint64_t bufferLength = found->bufferLength;
    found->completion = nullptr;
    found->buffer = nullptr;
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
        memcpy(reinterpret_cast<void *>(bufferAddress),
               reinterpret_cast<const uint8_t *>(bytes) + sizeof(ISCSIKitTaskResponse),
               copyLength);
    }
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

    setNumber(kIOMaximumSegmentCountReadKey, 256);
    setNumber(kIOMaximumSegmentCountWriteKey, 256);
    setNumber(kIOMaximumSegmentByteCountReadKey, kMaxTransferSize);
    setNumber(kIOMaximumSegmentByteCountWriteKey, kMaxTransferSize);
    setNumber(kIOMinimumSegmentAlignmentByteCountKey, 4);
    setNumber(kIOMaximumSegmentAddressableBitCountKey, 64);
    setNumber(kIOMinimumHBADataAlignmentMaskKey, 0xFFFFFFFFFFFFFFFF);

    kern_return_t ret = UserReportHBAConstraints(constraints);
    constraints->release();
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
    if (parallelRequest.fTransferDirection != kISCSIKitNoData &&
        parallelRequest.fRequestedTransferCount > 0) {
        kern_return_t ret = UserGetDataBuffer(parallelRequest.fTargetID,
                                              parallelRequest.fControllerTaskIdentifier,
                                              &buffer);
        if (ret != kIOReturnSuccess || !buffer) {
            *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        buffer->GetAddressRange(&range);
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
    slot->bufferAddress = range.address;
    slot->bufferLength = range.length;
    iSCSIKitUserClient * client = ivars->userClient;
    IOLockUnlock(ivars->lock);

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

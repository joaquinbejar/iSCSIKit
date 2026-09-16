// RAM-backed virtual SCSI HBA: one target, one 64 MiB LUN, no hardware.
// Every WRITE logs how many bytes of the payload handed over by
// UserGetDataBuffer are nonzero. On Apple Silicon macOS 26 that count is 0
// for every write, while reads through the same buffer work (FB24799838).
#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <SCSIControllerDriverKit/IOSCSIParallelControllerCharacteristics.h>
#include "RAMDiskDext.h"
#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "RAMDiskDext: " fmt, ##__VA_ARGS__)

static constexpr uint64_t kBlockSize = 512;
static constexpr uint64_t kDiskBytes = 64ULL * 1024 * 1024;
static constexpr uint64_t kMaxTransfer = 16384;   // one Apple Silicon page

struct RAMDiskDext_IVars {
    IODispatchQueue * targets;      // UserCreateTargetForID blocks until the probe completes...
    IODispatchQueue * completions;  // ...so completions must run on a different queue
    uint8_t * disk;
    uint32_t nextTaskID;
    // Published as IORegistry properties (ioreg -r -n RAMDiskDext -l): dext
    // os_log output is not reliably visible, the registry always is.
    uint64_t tasks, writes, writeBytes, writeNonzeroBytes, lastOpcode, lastStatus, createTargetResult;
};

static void publish(RAMDiskDext * dext, RAMDiskDext_IVars * v)
{
    OSDictionary * d = OSDictionary::withCapacity(8);
    if (!d) return;
    auto set = [&](const char * key, uint64_t value) {
        OSNumber * n = OSNumber::withNumber(value, 64);
        if (n) { d->setObject(key, n); n->release(); }
    };
    set("Tasks", v->tasks); set("Writes", v->writes); set("WriteBytes", v->writeBytes);
    set("WriteNonzeroBytes", v->writeNonzeroBytes); set("LastOpcode", v->lastOpcode);
    set("LastStatus", v->lastStatus); set("CreateTargetResult", v->createTargetResult);
    dext->SetProperties(d);
    d->release();
}

bool RAMDiskDext::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(RAMDiskDext_IVars, 1);
    return ivars != nullptr;
}

void RAMDiskDext::free()
{
    if (ivars) {
        if (ivars->disk) IOFree(ivars->disk, kDiskBytes);
        OSSafeReleaseNULL(ivars->targets);
        OSSafeReleaseNULL(ivars->completions);
        IOSafeDeleteNULL(ivars, RAMDiskDext_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(RAMDiskDext, Start)
{
    // The framework dispatches QUEUENAME(AuxiliaryQueue) methods onto a queue
    // the dext must create before the kernel connection exists.
    IODispatchQueue * auxiliary = nullptr;
    kern_return_t ret = IODispatchQueue::Create("AuxiliaryQueue", 0, 0, &auxiliary);
    if (ret != kIOReturnSuccess) return ret;
    ret = SetDispatchQueue("AuxiliaryQueue", auxiliary);
    auxiliary->release();
    if (ret != kIOReturnSuccess) return ret;
    ret = IODispatchQueue::Create("RAMDiskTargets", 0, 0, &ivars->targets);
    if (ret != kIOReturnSuccess) return ret;
    ret = IODispatchQueue::Create("RAMDiskCompletions", 0, 0, &ivars->completions);
    if (ret != kIOReturnSuccess) return ret;
    ivars->disk = reinterpret_cast<uint8_t *>(IOMallocZero(kDiskBytes));
    if (!ivars->disk) return kIOReturnNoMemory;
    ivars->nextTaskID = 1;
    return Start(provider, SUPERDISPATCH);
}

kern_return_t IMPL(RAMDiskDext, UserInitializeController)
{
    OSDictionary * c = OSDictionary::withCapacity(8);
    if (!c) return kIOReturnNoMemory;
    auto set = [&](const char * key, uint64_t value) {
        OSNumber * n = OSNumber::withNumber(value, 64);
        if (n) { c->setObject(key, n); n->release(); }
    };
    set(kIOMaximumSegmentCountReadKey, 1);
    set(kIOMaximumSegmentCountWriteKey, 1);
    set(kIOMaximumSegmentByteCountReadKey, kMaxTransfer);
    set(kIOMaximumSegmentByteCountWriteKey, kMaxTransfer);
    set(kIOMinimumSegmentAlignmentByteCountKey, 4);
    set(kIOMaximumSegmentAddressableBitCountKey, 64);
    set(kIOMinimumHBADataAlignmentMaskKey, 0xFFFFFFFFFFFFFFFF);
    kern_return_t ret = UserReportHBAConstraints(c);
    c->release();
    return ret;
}

kern_return_t IMPL(RAMDiskDext, UserStartController)
{
    // UserCreateTargetForID blocks until the target's probe I/O completes,
    // and that I/O is served by UserProcessParallelTask on the Default
    // queue, so it must not be called from a framework callback. Wait for
    // the controller start to settle in the kernel before creating it.
    retain();
    ivars->targets->DispatchAsync(^{
        IOSleep(2000);
        OSDictionary * props = OSDictionary::withCapacity(1);
        if (props) {
            ivars->createTargetResult = UserCreateTargetForID(0, props);
            LOG("create target 0: 0x%llx", ivars->createTargetResult);
            props->release();
            publish(this, ivars);
        }
        release();
    });
    return kIOReturnSuccess;
}

static uint64_t be(const uint8_t * p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }
static void put(uint8_t * p, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) { p[i] = uint8_t(v); v >>= 8; } }

kern_return_t IMPL(RAMDiskDext, UserProcessParallelTask)
{
    const uint8_t * cdb = parallelRequest.fCommandDescriptorBlock;
    uint64_t want = parallelRequest.fRequestedTransferCount;
    IOBufferMemoryDescriptor * buffer = nullptr;
    uint8_t * data = nullptr;
    uint64_t dataLength = 0;
    if (want > 0 && parallelRequest.fTransferDirection != 0) {
        // The only documented data access for a software controller.
        kern_return_t ret = UserGetDataBuffer(parallelRequest.fTargetID,
                                              parallelRequest.fControllerTaskIdentifier, &buffer);
        if (ret != kIOReturnSuccess || !buffer) {
            *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        IOAddressSegment range = {};
        buffer->GetAddressRange(&range);
        data = reinterpret_cast<uint8_t *>(range.address);
        dataLength = range.length < want ? range.length : want;
    }
    SCSIUserParallelResponse reply = {};
    reply.version = kScsiUserParallelTaskResponseCurrentVersion1;
    reply.fTargetID = parallelRequest.fTargetID;
    reply.fControllerTaskIdentifier = parallelRequest.fControllerTaskIdentifier;
    reply.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    reply.fCompletionStatus = kSCSITaskStatus_GOOD;
    uint8_t out[64] = {};
    uint64_t outLength = 0;
    auto illegal = [&](uint8_t asc) {
        reply.fCompletionStatus = kSCSITaskStatus_CHECK_CONDITION;
        reply.fSenseLength = 18;
        reply.fSenseBuffer[0] = 0x70; reply.fSenseBuffer[2] = 0x05;   // ILLEGAL REQUEST
        reply.fSenseBuffer[7] = 10;   reply.fSenseBuffer[12] = asc;
    };
    auto rw = [&](uint64_t lba, uint64_t blocks, bool write) {
        uint64_t offset = lba * kBlockSize, length = blocks * kBlockSize;
        if (offset + length > kDiskBytes || length > dataLength) { illegal(0x21); return; }   // LBA out of range
        if (write) {
            uint64_t nonzero = 0;
            for (uint64_t i = 0; i < length; i++) nonzero += data[i] != 0;
            ivars->writes++; ivars->writeBytes += length; ivars->writeNonzeroBytes += nonzero;
            // THE BUG: on Apple Silicon macOS 26 nonzero is always 0 here.
            LOG("WRITE lba %llu blocks %llu: %llu nonzero bytes in the buffer from UserGetDataBuffer",
                lba, blocks, nonzero);
            memcpy(ivars->disk + offset, data, length);
        } else {
            memcpy(data, ivars->disk + offset, length);
        }
        reply.fBytesTransferred = length;
    };
    switch (cdb[0]) {
    case 0x00: case 0x1E: case 0x35: case 0x91: break;                       // TUR, PREVENT/ALLOW, SYNC CACHE
    case 0x03: outLength = 18; out[0] = 0x70; break;                           // REQUEST SENSE: no sense
    case 0x12:                                                                 // INQUIRY
        if (cdb[1] & 1) { illegal(0x24); break; }                              // no VPD pages
        out[2] = 0x06; out[3] = 0x02; out[4] = 31; out[7] = 0x02;
        memcpy(out + 8, "APPLE   RAMDisk repro   0001", 28);
        outLength = 36;
        break;
    case 0x25: put(out, kDiskBytes / kBlockSize - 1, 4); put(out + 4, kBlockSize, 4); outLength = 8; break;
    case 0x9E:                                                                 // SERVICE ACTION IN(16)
        if ((cdb[1] & 0x1F) != 0x10) { illegal(0x24); break; }
        put(out, kDiskBytes / kBlockSize - 1, 8); put(out + 8, kBlockSize, 4); outLength = 32;
        break;
    case 0x1A: out[0] = 3; outLength = 4; break;                              // MODE SENSE(6): empty
    case 0x5A: out[1] = 6; outLength = 8; break;                              // MODE SENSE(10): empty
    case 0xA0: out[3] = 8; outLength = 16; break;                             // REPORT LUNS: LUN 0
    case 0x28: rw(be(cdb + 2, 4), be(cdb + 7, 2), false); break;             // READ(10)
    case 0x88: rw(be(cdb + 2, 8), be(cdb + 10, 4), false); break;            // READ(16)
    case 0x2A: rw(be(cdb + 2, 4), be(cdb + 7, 2), true); break;              // WRITE(10)
    case 0x8A: rw(be(cdb + 2, 8), be(cdb + 10, 4), true); break;             // WRITE(16)
    default: illegal(0x20); break;                                            // invalid opcode
    }
    if (outLength > 0 && data) {
        uint64_t n = outLength < dataLength ? outLength : dataLength;
        memcpy(data, out, n);
        reply.fBytesTransferred = n;
    }
    // Complete off the Default queue; the framework releases the buffer once
    // the completion callback has been invoked.
    SCSIUserParallelResponse * heap = IONew(SCSIUserParallelResponse, 1);   // blocks capture by value
    if (!heap) { OSSafeReleaseNULL(buffer); *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE; return kIOReturnNoMemory; }
    *heap = reply;
    ivars->tasks++; ivars->lastOpcode = cdb[0]; ivars->lastStatus = reply.fCompletionStatus;
    completion->retain();
    retain();
    ivars->completions->DispatchAsync(^{
        ParallelTaskCompletion(completion, *heap);
        publish(this, ivars);
        IODelete(heap, SCSIUserParallelResponse, 1);
        completion->release();
        if (buffer) buffer->release();
        release();
    });
    *response = kSCSIServiceResponse_Request_In_Process;
    return kIOReturnSuccess;
}

// Completions flow dext -> kernel; these target methods are never invoked here.
void IMPL(RAMDiskDext, ParallelTaskCompletion) { (void)action; (void)response; }
void IMPL(RAMDiskDext, BundledParallelTaskCompletion)
{ (void)action; (void)parallelResponseSlotIndices; (void)parallelResponseSlotIndicesCount; }

kern_return_t IMPL(RAMDiskDext, UserReportHBAHighestLogicalUnitNumber) { *value = 0; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserDoesHBASupportSCSIParallelFeature) { (void)theValue; *result = false; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserInitializeTargetForID) { (void)targetID; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserDoesHBAPerformAutoSense) { *result = true; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserDoesHBASupportMultiPathing) { *result = false; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserReportInitiatorIdentifier) { *id = 7; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserReportHighestSupportedDeviceID) { *id = 15; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserReportMaximumTaskCount) { *count = 16; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserDoesHBAPerformDeviceManagement) { *result = true; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserGetDMASpecification)
{
    *maxTransferSize = kMaxTransfer; *alignment = 4; *numAddressBits = 64;
    *segmentType = kDMAOutputSegmentHost64;
    return kIOReturnSuccess;
}
kern_return_t IMPL(RAMDiskDext, UserMapHBAData) { *uniqueTaskID = ivars->nextTaskID++; return kIOReturnSuccess; }
kern_return_t IMPL(RAMDiskDext, UserMapBundledParallelTaskCommandAndResponseBuffers)
{ (void)parallelCommandIOMemoryDescriptor; (void)parallelResponseIOMemoryDescriptor; return kIOReturnUnsupported; }
void IMPL(RAMDiskDext, UserProcessBundledParallelTasks)
{ (void)parallelRequestSlotIndices; (void)parallelRequestSlotIndicesCount; (void)completion; }
kern_return_t IMPL(RAMDiskDext, UserAbortTaskRequest) { (void)theT; (void)theL; (void)theQ; *response = 0; return kIOReturnUnsupported; }
kern_return_t IMPL(RAMDiskDext, UserAbortTaskSetRequest) { (void)theT; (void)theL; *response = 0; return kIOReturnUnsupported; }
kern_return_t IMPL(RAMDiskDext, UserClearACARequest) { (void)theT; (void)theL; *response = 0; return kIOReturnUnsupported; }
kern_return_t IMPL(RAMDiskDext, UserClearTaskSetRequest) { (void)theT; (void)theL; *response = 0; return kIOReturnUnsupported; }
kern_return_t IMPL(RAMDiskDext, UserLogicalUnitResetRequest) { (void)theT; (void)theL; *response = 0; return kIOReturnUnsupported; }
kern_return_t IMPL(RAMDiskDext, UserTargetResetRequest) { (void)theT; *response = 0; return kIOReturnUnsupported; }

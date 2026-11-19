// Wire protocol between iscsikitd (userspace daemon) and the iSCSIKit dext.
// Compiled into both sides; keep it C so Swift can import it unchanged.

#ifndef ISCSIKIT_PROTOCOL_H
#define ISCSIKIT_PROTOCOL_H

#include <stdint.h>

#define ISCSIKIT_SERVICE_NAME "iSCSIKitDext"

// One IOConnectCallStructMethod round trip carries at most this much payload.
// Must match the HBA's reported maximum segment byte count.
#define ISCSIKIT_MAX_TRANSFER (1024 * 1024)

#define ISCSIKIT_CDB_SIZE 16

// Must match the dext's slot count (kMaxTaskCount).
#define ISCSIKIT_MAX_TASKS 64
#define ISCSIKIT_LIST_PENDING_BYTES ((1 + 3 * ISCSIKIT_MAX_TASKS) * 8)
#define ISCSIKIT_SENSE_SIZE 256

enum ISCSIKitSelector {
    // Async: registers the "task pending" callback. asyncData[0] = controller
    // task id, asyncData[1] = dext sequence number.
    kISCSIKitMethodRegisterCallback = 0,
    // Scalars in: targetID, blockCount, blockSize. Publishes a SCSI target.
    kISCSIKitMethodRegisterTarget = 1,
    // Scalars in: targetID. Removes the target.
    kISCSIKitMethodUnregisterTarget = 2,
    // Scalar in: controller task id. Struct out: ISCSIKitTaskDescriptor
    // followed by dataOut payload for writes.
    kISCSIKitMethodDequeueTask = 3,
    // Struct in: ISCSIKitTaskResponse followed by dataIn payload for reads.
    kISCSIKitMethodCompleteTask = 4,
    // Scalar in: minimum age in ms. Struct out: uint64_t count, then count
    // triples of uint64_t (taskID, ageMs, seq), zero padded to
    // ISCSIKIT_LIST_PENDING_BYTES, for tasks queued but never dequeued for
    // that long, i.e. tasks whose "task pending" notification never reached
    // the daemon.
    kISCSIKitMethodListPending = 5,
    // Scalar in: index of the next event the caller wants. Struct out:
    // ISCSIKitEventBatchHeader followed by header.count ISCSIKitEvent, at
    // most ISCSIKIT_EVENT_BATCH. firstIndex > requested index means the ring
    // overwrote (firstIndex - requested) events before they were read.
    kISCSIKitMethodReadEvents = 6,
};

// Diagnostic trace: one event per step of a task's life in the dext. Every
// event carries the dext's own sequence number (seq), assigned on receipt,
// because controller task IDs and slots are reused.
#define ISCSIKIT_EVENT_RING 16384
#define ISCSIKIT_EVENT_BATCH 1024

enum ISCSIKitEventType {
    kISCSIKitEvReceived = 1,         // UserProcessParallelTask; value = requested bytes, code = direction
    kISCSIKitEvBufferFailed = 2,     // UserGetDataBuffer / staging failed; code = kern_return
    kISCSIKitEvNoClient = 3,         // no daemon connected; task failed
    kISCSIKitEvSlotReserved = 4,     // value = pending count after reserving
    kISCSIKitEvNoSlot = 5,           // all slots busy; task failed
    kISCSIKitEvNotifyOK = 6,         // AsyncCompletion issued (its delivery is not observable)
    kISCSIKitEvNotifyNoCallback = 7, // no callback registered; task failed
    kISCSIKitEvDequeueOK = 8,        // value = bytes sent to the daemon
    kISCSIKitEvDequeueFailed = 9,    // code = kern_return (underrun, nospace, mapping, nomem); task failed
    kISCSIKitEvDequeueNotFound = 10, // no pending slot with that task id; seq = 0
    kISCSIKitEvResponse = 11,        // daemon response; code = SCSI status, value = bytesTransferred
    kISCSIKitEvResponseNotFound = 12,// no in-flight slot with that task id; seq = 0
    kISCSIKitEvCompleted = 13,       // ParallelTaskCompletion; code = status, extra = service response, value = bytes
    kISCSIKitEvDisconnectFailed = 14,// daemon went away; task failed
};

typedef struct ISCSIKitEvent {
    uint64_t seq;
    uint64_t timeNs;   // CLOCK_UPTIME_RAW
    uint64_t taskID;
    uint64_t value;
    uint32_t code;
    uint8_t type;      // ISCSIKitEventType
    uint8_t extra;
    uint8_t reserved[2];
} ISCSIKitEvent;

typedef struct ISCSIKitEventBatchHeader {
    uint64_t firstIndex;  // index of the first event returned
    uint64_t count;
    uint64_t nextIndex;   // pass this back on the next call
} ISCSIKitEventBatchHeader;

enum ISCSIKitDirection {
    kISCSIKitNoData = 0,
    kISCSIKitWrite = 1,  // initiator -> target
    kISCSIKitRead = 2,   // target -> initiator
};

typedef struct ISCSIKitTaskDescriptor {
    uint64_t taskID;
    uint64_t targetID;
    uint64_t lun;
    uint32_t transferLength;
    uint8_t direction;  // ISCSIKitDirection
    uint8_t cdbLength;
    uint8_t cdb[ISCSIKIT_CDB_SIZE];
    uint8_t reserved[2];
    uint64_t seq;  // dext sequence number, for trace correlation
    // uint8_t dataOut[transferLength] follows when direction == kISCSIKitWrite
} ISCSIKitTaskDescriptor;

typedef struct ISCSIKitTaskResponse {
    uint64_t taskID;
    uint64_t targetID;
    uint64_t bytesTransferred;
    uint8_t status;           // SCSI status byte (0x00 GOOD, 0x02 CHECK CONDITION…)
    uint8_t senseLength;
    uint8_t sense[ISCSIKIT_SENSE_SIZE];
    uint8_t reserved[6];
    // uint8_t dataIn[bytesTransferred] follows when the task was a read
} ISCSIKitTaskResponse;

#endif /* ISCSIKIT_PROTOCOL_H */

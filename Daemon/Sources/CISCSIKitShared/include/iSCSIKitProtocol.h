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
#define ISCSIKIT_SENSE_SIZE 256

enum ISCSIKitSelector {
    // Async: registers the "task pending" callback. asyncData[0] = controller task id.
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
};

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

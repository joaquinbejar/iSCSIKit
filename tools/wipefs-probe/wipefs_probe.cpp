/*
 * wipefs_probe.cpp
 *
 * Standalone reproduction of libutil's wipefs_alloc() + wipefs_wipe(), with
 * logging, built for the iSCSIKit write-path investigation (Apple DTS case
 * 22255765). It answers one question: which pwrite() inside wipefs_wipe()
 * fails on the iSCSIKit LUN, with what return value and what errno.
 *
 * Default mode is a dry run: the device is opened O_RDONLY, only read-only
 * getter ioctls are issued, and the exact sequence of pwrite() calls wipefs
 * would make is printed. Nothing is written. DKIOCUNMAP and
 * DKIOCSYNCHRONIZECACHE are NOT issued in a dry run.
 *
 * --write (only together with --i-understand-this-erases) performs the same
 * I/O wipefs_wipe() performs, in the same order, and logs every call.
 *
 * ---------------------------------------------------------------------------
 * Attribution and license
 *
 * The ExtentManager class, the AddExtentsFor*() functions, query_disk_info(),
 * the size computations of wipefs_alloc() and the write loop of wipefs_wipe()
 * below are copied from Apple's libutil, published at
 *   https://github.com/apple-oss-distributions/libutil (branch main)
 * Files: wipefs.cpp (Copyright (c) 2008 Apple Inc.), ExtentManager.cpp
 * (Copyright (c) 2008-2009,2011 Apple Inc.), ExtentManager.h (Copyright (c)
 * 2008 Computer, Inc.), wipefs.h (Copyright (c) 2008 Apple Inc.). Those files
 * carry this license header, reproduced verbatim:
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 *
 * Modifications (this file is a Modification of Original Code under APSL
 * 2.0): every change to copied code is marked "PROBE:". The changes only add
 * logging, replace the single pwrite() call with a logging wrapper, and skip
 * the xartutil spawn. No arithmetic, type or control-flow change was made to
 * the copied computations.
 * ---------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <uuid/uuid.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOStorage.h>

#include <list>
#include <vector>
#include <string>
#include <algorithm>

// ===========================================================================
// Provenance of the mirrored source (recorded 2026-09-23)
// ===========================================================================

#define PROBE_VERSION              "wipefs_probe 1"
#define PROBE_SRC_REPO             "https://github.com/apple-oss-distributions/libutil"
#define PROBE_SRC_BRANCH           "main"
#define PROBE_SRC_HEAD             "0f4264d617b0a94d8905521fa444e2aa215f0a4d (libutil-73, 2025-10-16)"
#define PROBE_WIPEFS_LAST_COMMIT   "490b68051c46597b761a68cd2b43d21dee9432cd (libutil-70.1.1, 2023-09-22)"
#define PROBE_EXTMGR_LAST_COMMIT   "e916a2b2f19be81ebd1b7ce6432bc27ef9bf6de6 (libutil-30)"

struct ProbeSourceFile { const char *name; const char *sha256; const char *gitBlob; };
static const ProbeSourceFile kProbeSources[] = {
    { "wipefs.cpp",        "fc948607abf3a02984568bc57747e031dcaf72161126a7eb87d5f46d9f048b2c", "d8e63392fe4113ff50f008a1556e21859329ebd8" },
    { "wipefs.h",          "da725c8b798850e71a0995eef74ac4b94a22a96ce6d33c87dbb470b23b02c011", "0a9321b5d43e4f82d4074892466d00415ef02e87" },
    { "ExtentManager.cpp", "722fb93a7c121505522e1335c546fd085200ccfd2180e968490370671ad142b4", "905b4325a4ab485a5a04c311d0a2d70d700500cb" },
    { "ExtentManager.h",   "30f87eac22ca28c8f67c3a1c7b1020cc0f0f9e33da5d8add1d287b06f1167fbc", "2dc25f6ec4da12ad45063122ba7adf40fbe450cb" },
};

// The installed binary this logic was compared against (disassembly of
// wipefs_alloc, wipefs_wipe and ExtentManager with `dyld_info -disassemble`).
#define PROBE_CHECKED_LIBUTIL_UUID     "38245F99-D78C-38F1-94EE-22DF75E72ADE"
#define PROBE_CHECKED_LIBUTIL_SRCVERS  "76.0"
#define PROBE_CHECKED_OS               "macOS 27.0 (26A428)"

// libutil exports these (see usr/lib/libutil.tbd) but ships no header. The
// probe NEVER calls them; it only takes their address so dyld loads the
// installed libutil and the probe can report which build is on this Mac.
extern "C" int wipefs_alloc(int fd, size_t block_size, void **handle);
extern "C" int wipefs_wipe(void *handle);
extern "C" void wipefs_free(void **handle);
// Exception: --selftest calls the INSTALLED wipefs_alloc()/wipefs_free() on a
// REGULAR FILE (fstat + memory only, no I/O) to compare its extent list with
// the probe's. wipefs_wipe() is never called.

// ===========================================================================
// PROBE: logging (errno-preserving, optional fsync'd log file)
// ===========================================================================

static int      gLogFd = -1;
static bool     gQuiet = false;   // suppresses plog() while --selftest runs the probe's alloc
static uint64_t gT0 = 0;
static mach_timebase_info_data_t gTimebase;

static double probe_now(void)
{
    uint64_t t = mach_absolute_time() - gT0;
    return (double)t * gTimebase.numer / gTimebase.denom / 1e9;
}

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...)
{
    if (gQuiet) return;
    int saved = errno;   // logging must never disturb the errno being studied
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char out[4200];
    int n = snprintf(out, sizeof(out), "[%11.6f] %s\n", probe_now(), line);
    if (n > (int)sizeof(out) - 1) n = (int)sizeof(out) - 1;
    fwrite(out, 1, (size_t)n, stdout);
    fflush(stdout);
    if (gLogFd >= 0) {
        (void)write(gLogFd, out, (size_t)n);
        (void)fsync(gLogFd);   // survive a kernel panic as far as possible
    }
    errno = saved;
}

static const char *errname(int e)
{
    switch (e) {
    case 0: return "0";
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case EIO: return "EIO";
    case ENXIO: return "ENXIO";
    case EBADF: return "EBADF";
    case ENOMEM: return "ENOMEM";
    case EACCES: return "EACCES";
    case EFAULT: return "EFAULT";
    case EBUSY: return "EBUSY";
    case ENODEV: return "ENODEV";
    case EINVAL: return "EINVAL";
    case ENOTTY: return "ENOTTY";
    case EFBIG: return "EFBIG";
    case ENOSPC: return "ENOSPC";
    case EROFS: return "EROFS";
    case ENOTSUP: return "ENOTSUP";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EOVERFLOW: return "EOVERFLOW";
    default: return "?";
    }
}

// ===========================================================================
// ExtentManager: copied from libutil ExtentManager.h / ExtentManager.cpp
// ===========================================================================

using namespace std;

struct ExtentInfo {
	off_t blockAddr;
	off_t numBlocks;
};

inline bool BeforeExtent(const ExtentInfo &a, const ExtentInfo &b)
{
		return (a.blockAddr + a.numBlocks) < b.blockAddr;
}

typedef list<ExtentInfo>::iterator ListExtIt;

// PROBE: record every range request (which filesystem asked for what), for
// display only. Filled by the two Add*Extent methods below.
struct ProbeRangeRequest {
	std::string fs;
	bool byteRange;          // true: AddByteRangeExtent, false: AddBlockRangeExtent
	off_t addr;              // bytes or blocks, as passed in
	off_t count;             // bytes or blocks, as passed in
	off_t blockAddr;         // resulting block range before the ExtentManager clip
	off_t numBlocks;
};
static std::vector<ProbeRangeRequest> gProbeRequests;
static const char *gProbeCurrentFS = "?";
static bool gProbeInByteRange = false;

class ExtentManager {
public:
	ExtentManager() : blockSize(0), totalBytes(0), totalBlocks(0) {};
	~ExtentManager() {};

	void Init(uint32_t theBlockSize, uint32_t theNativeBlockSize, off_t theTotalBytes);

	void AddBlockRangeExtent(off_t blockAddr, off_t numBlocks);
	void AddByteRangeExtent(off_t byteAddr, off_t numBytes);

protected:
	void MergeExtent(const ExtentInfo &a, const ExtentInfo &b, ExtentInfo *c);

public:
	size_t blockSize;
	size_t nativeBlockSize;
	off_t totalBytes;
	off_t totalBlocks;
	list<ExtentInfo> extentList;
};

void
ExtentManager::Init(uint32_t theBlockSize, uint32_t theNativeBlockSize, off_t theTotalBytes)
{
	blockSize = theBlockSize;
	nativeBlockSize = theNativeBlockSize;
	totalBytes = theTotalBytes;
	totalBlocks = howmany(totalBytes, blockSize);

	// add sentry empty extents at both sides so empty partition doesn't need to be handled specially
	gProbeCurrentFS = "sentinel (ExtentManager::Init)";   // PROBE
	AddBlockRangeExtent(0, 0);
	AddBlockRangeExtent(totalBlocks, 0);
}

void
ExtentManager::MergeExtent(const ExtentInfo &a, const ExtentInfo &b, ExtentInfo *c)
{
	// merge ext into *curIt
	c->blockAddr = min(a.blockAddr, b.blockAddr);
	c->numBlocks = max(a.blockAddr + a.numBlocks, b.blockAddr + b.numBlocks) - c->blockAddr;
}

void
ExtentManager::AddBlockRangeExtent(off_t blockAddr, off_t numBlocks)
{
	struct ExtentInfo ext, newExt;
	ListExtIt curIt, newIt;
	bool merged = false;

	if (!gProbeInByteRange) {   // PROBE: record direct block-range requests
		gProbeRequests.push_back({gProbeCurrentFS, false, blockAddr, numBlocks, blockAddr, numBlocks});
	}

	// make the range a valid range
	if ((blockAddr > totalBlocks) || (blockAddr + numBlocks < 0)) { // totally out of range, do nothing
		return;
	}
	if (blockAddr < 0) {
		numBlocks = blockAddr + numBlocks;
		blockAddr = 0;
	}
	if (blockAddr + numBlocks > totalBlocks) {
		numBlocks = totalBlocks - blockAddr;
	}

	ext.blockAddr = blockAddr;
	ext.numBlocks = numBlocks;

	for (curIt = extentList.begin(); curIt != extentList.end(); curIt++) {
		if (BeforeExtent(ext, *curIt))
			break;
		if (!BeforeExtent(*curIt, ext)) { // overlapped extents
			MergeExtent(ext, *curIt, &newExt);
			*curIt = newExt;
			merged = true;
			break;
		}
	}

	// insert ext before curIt
	if (!merged) {
		curIt = extentList.insert(curIt, ext); // throws bad_alloc when out of memory
	}

	// merge the extents
	newIt = curIt;
	curIt = extentList.begin();
	while (curIt != extentList.end()) {
		if (curIt == newIt || BeforeExtent(*curIt, *newIt)) { // curIt is before newIt
			curIt++;
			continue;
		}
		if (BeforeExtent(*newIt, *curIt)) { // curIt is after newIt now, we are done
			break;
		}
		// merge the two extents
		MergeExtent(*curIt, *newIt, &newExt);
		*newIt = newExt;
		curIt = extentList.erase(curIt);
	}
} // ExtentManager::AddBlockRangeExtent

void
ExtentManager::AddByteRangeExtent(off_t byteAddr, off_t numBytes)
{
	off_t blockAddr = byteAddr / blockSize;
	off_t blockAddrOfLastByte = (byteAddr + numBytes - 1) / blockSize;
	off_t numBlocks = blockAddrOfLastByte - blockAddr + 1;
	gProbeRequests.push_back({gProbeCurrentFS, true, byteAddr, numBytes, blockAddr, numBlocks});   // PROBE
	gProbeInByteRange = true;    // PROBE
	AddBlockRangeExtent(blockAddr, numBlocks);
	gProbeInByteRange = false;   // PROBE
}

// ===========================================================================
// AddExtentsFor*: copied verbatim from libutil wipefs.cpp
// ===========================================================================

#define	wipefs_roundup(x, y)	((((x)+((y)-1))/(y))*(y))

static void
AddExtentsForFutureFS(class ExtentManager *extMan)
{
	// we don't know what blocks future FS will use to recognize itself.  But we'd better be safe than sorry and write
	// the first and last 2MB of the volume
	off_t size = 2 * 1024 * 1024;
	extMan->AddByteRangeExtent(0, size);
	extMan->AddByteRangeExtent(extMan->totalBytes - size, size);
}

static void
AddExtentsForHFS(class ExtentManager *extMan)
{
	// first 1KB is boot block, last 512B is reserved
	// the Volume Header (512B) is after 1KB and before the last 512B
	extMan->AddByteRangeExtent(0, 1024 + 512);
	extMan->AddByteRangeExtent(extMan->totalBytes - 1024, 1024);
}

static void
AddExtentsForMSDOS(class ExtentManager *extMan)
{
	// MSDOS needs the first block (in theory, up to 32KB)
	extMan->AddByteRangeExtent(0, 32 * 1024);
}

static void
AddExtentsForNTFS(class ExtentManager *extMan)
{
	// NTFS supports block size from 256B to 32768B.  The first, middle and last block are needed
	extMan->AddByteRangeExtent(0, 32 * 1024);
	extMan->AddByteRangeExtent(extMan->totalBytes - 32 * 1024, 32 * 1024);
	// to be safe, add the rage from (mid_point - 32KB) to (mid_point + 32KB)
	extMan->AddByteRangeExtent(extMan->totalBytes / 2 - 32 * 1024, 64 * 1024);
}

static void
AddExtentsForUDF(class ExtentManager *extMan)
{
	off_t lastBlockAddr = extMan->totalBlocks - 1;

	// Volume Recognization Sequence (VRS) starts at 32KB, usually less than 7 Volume Structure Descriptors (2KB each)
	extMan->AddByteRangeExtent(32 * 1024, 14 * 1024);

	// AVDP is on 256, 512, last block, last block - 256
	extMan->AddBlockRangeExtent(256, 1);
	extMan->AddBlockRangeExtent(512, 1);
	extMan->AddBlockRangeExtent(lastBlockAddr, 1);
	extMan->AddBlockRangeExtent(lastBlockAddr - 256, 1);

	// to be safe, assume the device has 2KB block size and do it again
	if (extMan->blockSize != 2048) {
		off_t blockSize = 2048;
		// AVDP is on 256, 512, last block, last block - 256
		extMan->AddByteRangeExtent(256 * blockSize, blockSize);
		extMan->AddByteRangeExtent(512 * blockSize, blockSize);
		extMan->AddByteRangeExtent(extMan->totalBytes - blockSize, blockSize);
		extMan->AddByteRangeExtent(extMan->totalBytes - 256 * blockSize, blockSize);
	}
}

static void
AddExtentsForUFS(class ExtentManager *extMan)
{
	// UFS super block is 8KB at offset 8KB
	extMan->AddByteRangeExtent(8192, 8192);
}

static void
AddExtentsForZFS(class ExtentManager *extMan)
{
	// ZFS needs the first 512KB and last 512KB for all the 4 disk labels
	extMan->AddByteRangeExtent(0, 512 * 1024);
	extMan->AddByteRangeExtent(extMan->totalBytes - 512 * 1024, 512 * 1024);
}

static void
AddExtentsForPartitions(class ExtentManager *extMan)
{
	// MBR (Master Boot Record) needs the first sector
	// APM (Apple Partition Map) needs the second sector
	// GPT (GUID Partition Table) needs the first 34 and last 33 sectors
	extMan->AddByteRangeExtent(0, 512 * 34);
	extMan->AddByteRangeExtent(extMan->totalBytes - 512 * 33, 512 * 33);
}

static void
AddExtentsForCoreStorage(class ExtentManager *extMan)
{
	// the CoreStorage VolumeHeader structures reside in the first/last 512 bytes of each PV
	extMan->AddByteRangeExtent(0, 512);
	extMan->AddByteRangeExtent(extMan->totalBytes - 512, 512);
}

// Copied verbatim from wipefs.cpp: derives the "diskN" name that wipefs
// passes to `/usr/sbin/xartutil --erase-disk`.
static char *
query_disk_info(int fd) {
	char disk_path[PATH_MAX];
	char *disk_name;

	// Fetch the path
	if (fcntl(fd, F_GETPATH, disk_path) == -1) {
		return (NULL);
	}

	// Find the last pathname component.
	disk_name = strrchr(disk_path, '/');
	if (disk_name == NULL) {
		// Not that we expect this to happen...
		disk_name = disk_path;
	} else {
		// Skip over the '/'.
		disk_name++;
	}

	if (*disk_name == 'r') {
		// Raw device; skip over leading 'r'.
		disk_name++;
	}

	// ...and make sure it's really a disk.
	if (strncmp(disk_name, "disk", strlen("disk")) != 0) {
		return (NULL);
	}

	return (strdup(disk_name));
}

// ===========================================================================
// PROBE: options and small helpers
// ===========================================================================

struct Options {
	const char *device = nullptr;
	bool write = false;
	bool iUnderstand = false;
	bool allowLarge = false;
	bool storagekitdFollowup = false;
	bool skipUnmap = false;
	bool shlock = false;
	size_t wipefsBlockSize = 0;       // wipefs_alloc(fd, block_size, ...): 0 = native
	bool haveWipefsBlockSize = false;
	bool simulate = false;
	uint32_t simBlockSize = 0;
	uint64_t simBlockCount = 0;
	uint64_t splitBytes = 0;          // expected max bytes per SCSI command (display only)
	const char *logPath = nullptr;
	bool allowNonIscsikit = false;    // --write on a LUN with no iSCSIKit ancestor
	bool help = false;
	bool provenance = false;
	const char *selftestPath = nullptr;
};

static const off_t kLargeDeviceLimit = (off_t)64 * 1024 * 1024 * 1024;   // 64 GiB

static void print_usage(FILE *f)
{
	fprintf(f,
"usage:\n"
"  wipefs_probe --device /dev/rdiskN [--dry-run] [--allow-large] [--log FILE]\n"
"  wipefs_probe --device /dev/rdiskN --write --i-understand-this-erases\n"
"               [--storagekitd-followup] [--skip-unmap] [--shlock]\n"
"               [--allow-non-iscsikit] [--log FILE]\n"
"  wipefs_probe --simulate BLOCKSIZE:BLOCKCOUNT [--split-bytes N]\n"
"  wipefs_probe --selftest REGULAR_FILE\n"
"  wipefs_probe --provenance | --help\n"
"\n"
"Reproduces libutil wipefs_alloc() + wipefs_wipe() with logging.\n"
"\n"
"  --device PATH            whole-disk raw device (/dev/rdiskN). In dry-run mode a\n"
"                           regular file is accepted too (wipefs's S_IFREG path).\n"
"  --dry-run                default. Opens O_RDONLY, issues only read-only getter\n"
"                           ioctls, prints every pwrite wipefs WOULD issue. Writes\n"
"                           nothing; DKIOCUNMAP and DKIOCSYNCHRONIZECACHE are not sent.\n"
"  --write                  perform wipefs_wipe's I/O for real (DESTROYS DATA: the first\n"
"                           and last 2 MiB and 64 KiB around the middle are zeroed and,\n"
"                           if the device supports it, the WHOLE device is unmapped).\n"
"                           Requires --i-understand-this-erases.\n"
"  --allow-non-iscsikit     permit --write on a disk whose IORegistry ancestry has no\n"
"                           iSCSIKit / IOUserSCSIParallelInterfaceController (refused\n"
"                           by default).\n"
"  --allow-large            permit a device larger than 64 GiB (refused by default).\n"
"  --wipefs-block-size N    block_size argument of wipefs_alloc (default: the\n"
"                           DKIOCGETBLOCKSIZE value, which is what storagekitd passes).\n"
"                           A power of two in [512, 2^31].\n"
"  --storagekitd-followup   after a wipefs failure (or a pwrite count mismatch), also do what storagekitd's\n"
"                           -[DMToolEraseDisk wipeDisk:shared:] does: pwrite one block\n"
"                           at offset 0 and one at totalBytes - blockSize.\n"
"  --skip-unmap             NOT faithful: skip DKIOCUNMAP (A/B test of stale errno).\n"
"  --shlock                 open with O_RDWR|O_SHLOCK (storagekitd's 'shared' path).\n"
"  --simulate BS:COUNT      no device at all: compute the plan for a disk that reports\n"
"                           DKIOCGETBLOCKSIZE=BS and DKIOCGETBLOCKCOUNT=COUNT.\n"
"  Numbers are decimal or 0x hex; a sign or a leading 0 (octal) is rejected.\n"
"  --split-bytes N          show how each pwrite would split into SCSI commands of at\n"
"                           most N bytes (default: DKIOCGETMAXBYTECOUNTWRITE).\n"
"  --log FILE               also append every line to FILE, fsync'd per line.\n"
"  --selftest FILE          call the INSTALLED libutil wipefs_alloc() on a regular file\n"
"                           (no I/O) and compare its extent list with the probe's.\n"
"  --provenance             print the provenance of the mirrored source and exit.\n");
}

// Unsigned decimal, or hex with an explicit 0x prefix. Rejects a sign,
// leading whitespace, a leading 0 (strtoull base 0 would read it as octal),
// trailing garbage and overflow.
static bool parse_u64(const char *s, uint64_t *out)
{
	if (!s || !*s) return false;
	int base = 10;
	const char *digits = s;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		digits = s + 2;
		if (!isxdigit((unsigned char)*digits)) return false;
	} else if (!isdigit((unsigned char)*digits) || (digits[0] == '0' && digits[1] != '\0')) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	unsigned long long v = strtoull(digits, &end, base);
	if (errno != 0 || !end || *end != '\0') return false;
	*out = v;
	return true;
}

// --wipefs-block-size and the BLOCKSIZE of --simulate: a power of two in
// [512, 2^31] so the uint32_t casts in ExtentManager::Init never yield 0.
static bool valid_block_size(uint64_t v)
{
	return v >= 512 && v <= ((uint64_t)1 << 31) && (v & (v - 1)) == 0;
}

// "/dev/rdisk15" -> "disk15"; returns false for anything that is not
// /dev/diskN or /dev/rdiskN (optionally with sM slices when allowSlices).
static bool bsd_name_from_path(const char *path, bool allowSlices, std::string *bsd)
{
	if (strncmp(path, "/dev/", 5) != 0) return false;
	const char *p = path + 5;
	if (*p == 'r') p++;
	if (strncmp(p, "disk", 4) != 0) return false;
	const char *q = p + 4;
	if (!isdigit((unsigned char)*q)) return false;
	while (isdigit((unsigned char)*q)) q++;
	if (*q != '\0') {
		if (!allowSlices) return false;
		while (*q == 's') {
			q++;
			if (!isdigit((unsigned char)*q)) return false;
			while (isdigit((unsigned char)*q)) q++;
		}
		if (*q != '\0') return false;
	}
	*bsd = std::string(p);
	return true;
}

static void print_provenance(void)
{
	plog("PROVENANCE mirrored source: %s branch %s", PROBE_SRC_REPO, PROBE_SRC_BRANCH);
	plog("PROVENANCE   branch head at fetch time: %s", PROBE_SRC_HEAD);
	plog("PROVENANCE   last commit touching wipefs.cpp: %s", PROBE_WIPEFS_LAST_COMMIT);
	plog("PROVENANCE   last commit touching ExtentManager.cpp: %s", PROBE_EXTMGR_LAST_COMMIT);
	for (const auto &f : kProbeSources) {
		plog("PROVENANCE   %-18s sha256 %s git-blob %s", f.name, f.sha256, f.gitBlob);
	}
	plog("PROVENANCE logic compared by disassembly against installed libutil UUID %s,"
	     " LC_SOURCE_VERSION %s, on %s: same ioctls, same extents, same loop"
	     " (the installed wipefs_wipe additionally returns EINVAL for a NULL handle).",
	     PROBE_CHECKED_LIBUTIL_UUID, PROBE_CHECKED_LIBUTIL_SRCVERS, PROBE_CHECKED_OS);
	plog("PROVENANCE the published source is NOT guaranteed to match the libutil installed on"
	     " the machine that runs this probe; compare the runtime line below.");
}

// Reports the UUID and LC_SOURCE_VERSION of the libutil dyld actually loaded.
static void print_installed_libutil(void)
{
	volatile void *keep1 = (void *)&wipefs_alloc;   // forces dyld to bind libutil
	volatile void *keep2 = (void *)&wipefs_wipe;    // never called
	(void)keep1; (void)keep2;

	char osv[64] = {0}, osb[64] = {0};
	size_t l1 = sizeof(osv), l2 = sizeof(osb);
	(void)sysctlbyname("kern.osproductversion", osv, &l1, nullptr, 0);
	(void)sysctlbyname("kern.osversion", osb, &l2, nullptr, 0);

	bool found = false;
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		if (!name || strcmp(name, "/usr/lib/libutil.dylib") != 0) continue;
		found = true;
		const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
		char uuidStr[40] = "unknown";
		char srcVers[64] = "unknown";
		const uint8_t *cmd = (const uint8_t *)(mh + 1);
		for (uint32_t c = 0; c < mh->ncmds; c++) {
			const struct load_command *lc = (const struct load_command *)cmd;
			if (lc->cmd == LC_UUID) {
				const struct uuid_command *uc = (const struct uuid_command *)lc;
				uuid_unparse_upper(uc->uuid, uuidStr);
			} else if (lc->cmd == LC_SOURCE_VERSION) {
				const struct source_version_command *sv = (const struct source_version_command *)lc;
				uint64_t v = sv->version;   // A.B.C.D.E packed 24.10.10.10.10
				unsigned a = (unsigned)(v >> 40), b = (unsigned)((v >> 30) & 0x3ff),
				         cc = (unsigned)((v >> 20) & 0x3ff);
				snprintf(srcVers, sizeof(srcVers), "%u.%u.%u", a, b, cc);
			}
			cmd += lc->cmdsize;
		}
		bool same = strcmp(uuidStr, PROBE_CHECKED_LIBUTIL_UUID) == 0;
		plog("RUNTIME os %s (%s); installed /usr/lib/libutil.dylib UUID %s source-version %s: %s",
		     osv, osb, uuidStr, srcVers,
		     same ? "SAME binary the probe logic was checked against"
		          : "DIFFERENT binary from the one checked; re-verify with dyld_info -disassemble");
	}
	if (!found) {
		plog("RUNTIME os %s (%s); /usr/lib/libutil.dylib not found among loaded images", osv, osb);
	}
}

// ===========================================================================
// PROBE: IORegistry (what the storage stack was told about the device)
// ===========================================================================

static std::string cf_to_string(CFTypeRef v)
{
	if (!v) return "(absent)";
	CFTypeID t = CFGetTypeID(v);
	char buf[512];
	if (t == CFNumberGetTypeID()) {
		long long n = 0;
		CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
		snprintf(buf, sizeof(buf), "%lld (0x%llx)", n, (unsigned long long)n);
		return buf;
	}
	if (t == CFBooleanGetTypeID()) return CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false";
	if (t == CFStringGetTypeID()) {
		if (!CFStringGetCString((CFStringRef)v, buf, sizeof(buf), kCFStringEncodingUTF8)) return "(string)";
		return std::string("\"") + buf + "\"";
	}
	if (t == CFDataGetTypeID()) {
		snprintf(buf, sizeof(buf), "<data %ld bytes>", (long)CFDataGetLength((CFDataRef)v));
		return buf;
	}
	if (t == CFDictionaryGetTypeID()) {
		std::string s = "{";
		CFIndex n = CFDictionaryGetCount((CFDictionaryRef)v);
		std::vector<const void *> keys((size_t)n), vals((size_t)n);
		CFDictionaryGetKeysAndValues((CFDictionaryRef)v, keys.data(), vals.data());
		for (CFIndex i = 0; i < n; i++) {
			char k[256] = "?";
			if (CFGetTypeID(keys[(size_t)i]) == CFStringGetTypeID())
				CFStringGetCString((CFStringRef)keys[(size_t)i], k, sizeof(k), kCFStringEncodingUTF8);
			CFTypeID vt = CFGetTypeID(vals[(size_t)i]);
			std::string vs = (vt == CFDictionaryGetTypeID()) ? std::string("{...}") : cf_to_string(vals[(size_t)i]);
			s += std::string(i ? ", " : "") + k + "=" + vs;
		}
		return s + "}";
	}
	return "(other CF type)";
}

static const char *kLimitKeys[] = {
	"IOMaximumByteCountRead", "IOMaximumByteCountWrite",
	"IOMaximumBlockCountRead", "IOMaximumBlockCountWrite",
	"IOMaximumSegmentCountRead", "IOMaximumSegmentCountWrite",
	"IOMaximumSegmentByteCountRead", "IOMaximumSegmentByteCountWrite",
	"IOMinimumSegmentAlignmentByteCount", "IOMaximumSegmentAddressableBitCount",
	"IOCommandPoolSize",
};

struct RegistryFacts {
	bool found = false;
	long long mediaSize = -1;
	long long preferredBlockSize = -1;
	bool iscsikitAncestor = false;   // walk reached iSCSIKit / IOUserSCSIParallelInterfaceController
	std::string ancestorClass;
};

// Reads property `k` of `entry`; logs it (always when logAbsent, else only if
// present) with `fmt` (two %s: key, value) and returns it retained, or NULL.
static CFTypeRef log_property(io_registry_entry_t entry, const char *k, const char *fmt, bool logAbsent)
{
	CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
	CFTypeRef v = IORegistryEntryCreateCFProperty(entry, key, nullptr, 0);
	CFRelease(key);
	if (v || logAbsent) plog(fmt, k, cf_to_string(v).c_str());
	return v;
}

static RegistryFacts dump_registry(const std::string &bsd, const char *when)
{
	RegistryFacts facts;
	plog("IOREG (%s) BSD name %s", when, bsd.c_str());
	CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault, 0, bsd.c_str());
	io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, match);   // consumes match
	if (!media) {
		plog("IOREG   no IOService with BSD name %s", bsd.c_str());
		return facts;
	}
	facts.found = true;
	io_name_t cls;
	IOObjectGetClass(media, cls);
	plog("IOREG   [0] %s", cls);
	const char *mediaKeys[] = { "Size", "Preferred Block Size", "Whole", "Writable", "Removable", "Content", "Leaf" };
	for (const char *k : mediaKeys) {
		CFTypeRef v = log_property(media, k, "IOREG       %-22s = %s", true);
		if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
			long long n = 0;
			CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
			if (strcmp(k, "Size") == 0) facts.mediaSize = n;
			if (strcmp(k, "Preferred Block Size") == 0) facts.preferredBlockSize = n;
		}
		if (v) CFRelease(v);
	}

	// Walk up the service plane to the controller and print, on the entry that
	// carries them, the limits and the block storage statistics.
	io_registry_entry_t cur = media;
	IOObjectRetain(cur);
	for (int depth = 1; depth <= 14; depth++) {
		io_registry_entry_t parent = MACH_PORT_NULL;
		if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) != KERN_SUCCESS) break;
		IOObjectRelease(cur);
		cur = parent;
		IOObjectGetClass(cur, cls);
		plog("IOREG   [%d] %s", depth, cls);
		for (const char *k : kLimitKeys) {
			CFTypeRef v = log_property(cur, k, "IOREG       %-36s = %s", false);
			if (v) CFRelease(v);
		}
		const char *dictKeys[] = { "Device Characteristics", "Protocol Characteristics", "Statistics" };
		for (const char *k : dictKeys) {
			CFTypeRef v = log_property(cur, k, "IOREG       %s = %s", false);
			if (v) CFRelease(v);
		}
		if (strcmp(cls, "IOUserSCSIParallelInterfaceController") == 0 ||
		    strstr(cls, "iSCSIKit") != nullptr) {
			facts.iscsikitAncestor = true;
			facts.ancestorClass = cls;
			break;
		}
	}
	IOObjectRelease(cur);
	IOObjectRelease(media);
	plog("IOREG   iSCSIKit ancestor: %s", facts.iscsikitAncestor
	     ? (std::string("found (") + facts.ancestorClass + ")").c_str() : "NOT found");
	return facts;
}

// ===========================================================================
// PROBE: getter ioctls
// ===========================================================================

struct IoctlResult { int ret; int err; uint64_t value; };

template <typename T>
static IoctlResult get(int fd, unsigned long req)
{
	T v = 0;
	errno = 0;
	int r = ioctl(fd, req, (char *)&v);
	int e = errno;
	return { r, e, (uint64_t)v };
}

static void log_ioctl(const char *name, const IoctlResult &r, const char *note)
{
	if (r.ret == 0) {
		plog("IOCTL %-40s ret=0 value=%llu (0x%llx)%s%s", name, (unsigned long long)r.value,
		     (unsigned long long)r.value, note ? "  " : "", note ? note : "");
	} else {
		plog("IOCTL %-40s ret=%d errno=%d %s (%s)%s%s", name, r.ret, r.err, errname(r.err), strerror(r.err),
		     note ? "  " : "", note ? note : "");
	}
}

struct ExtraLimits { uint64_t maxByteCountWrite = 0; uint32_t features = 0; bool haveFeatures = false; };

// Read-only getters that wipefs does NOT call, printed so the numbers the BSD
// layer sees can be checked against the IORegistry and the dext's settings.
static ExtraLimits extra_ioctls(int fd)
{
	ExtraLimits lim;
	struct { const char *name; unsigned long req; int size; } getters[] = {
		{ "DKIOCGETPHYSICALBLOCKSIZE",            DKIOCGETPHYSICALBLOCKSIZE,            4 },
		{ "DKIOCGETMAXBYTECOUNTREAD",             DKIOCGETMAXBYTECOUNTREAD,             8 },
		{ "DKIOCGETMAXBYTECOUNTWRITE",            DKIOCGETMAXBYTECOUNTWRITE,            8 },
		{ "DKIOCGETMAXBLOCKCOUNTREAD",            DKIOCGETMAXBLOCKCOUNTREAD,            8 },
		{ "DKIOCGETMAXBLOCKCOUNTWRITE",           DKIOCGETMAXBLOCKCOUNTWRITE,           8 },
		{ "DKIOCGETMAXSEGMENTCOUNTREAD",          DKIOCGETMAXSEGMENTCOUNTREAD,          8 },
		{ "DKIOCGETMAXSEGMENTCOUNTWRITE",         DKIOCGETMAXSEGMENTCOUNTWRITE,         8 },
		{ "DKIOCGETMAXSEGMENTBYTECOUNTREAD",      DKIOCGETMAXSEGMENTBYTECOUNTREAD,      8 },
		{ "DKIOCGETMAXSEGMENTBYTECOUNTWRITE",     DKIOCGETMAXSEGMENTBYTECOUNTWRITE,     8 },
		{ "DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT", DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT, 8 },
		{ "DKIOCGETMAXSEGMENTADDRESSABLEBITCOUNT",DKIOCGETMAXSEGMENTADDRESSABLEBITCOUNT,8 },
		{ "DKIOCGETCOMMANDPOOLSIZE",              DKIOCGETCOMMANDPOOLSIZE,              4 },
		{ "DKIOCISWRITABLE",                      DKIOCISWRITABLE,                      4 },
		{ "DKIOCGETFEATURES",                     DKIOCGETFEATURES,                     4 },
	};
	plog("EXTRA read-only getters (NOT called by wipefs; for cross-checking only):");
	for (const auto &g : getters) {
		IoctlResult r = g.size == 4 ? get<uint32_t>(fd, g.req) : get<uint64_t>(fd, g.req);
		const char *note = nullptr;
		char nb[128];
		if (g.req == DKIOCGETFEATURES && r.ret == 0) {
			lim.features = (uint32_t)r.value;
			lim.haveFeatures = true;
			snprintf(nb, sizeof(nb), "DK_FEATURE_UNMAP=%d DK_FEATURE_BARRIER=%d DK_FEATURE_PRIORITY=%d",
			         (lim.features & DK_FEATURE_UNMAP) ? 1 : 0, (lim.features & DK_FEATURE_BARRIER) ? 1 : 0,
			         (lim.features & DK_FEATURE_PRIORITY) ? 1 : 0);
			note = nb;
		}
		if (g.req == DKIOCGETMAXBYTECOUNTWRITE && r.ret == 0) lim.maxByteCountWrite = r.value;
		log_ioctl(g.name, r, note);
	}
	return lim;
}

// ===========================================================================
// PROBE: wipefs_alloc reproduction
// ===========================================================================

struct ProbeCtx {
	int fd = -1;
	ExtentManager extMan;
	char *diskname = nullptr;
	bool isRegularFile = false;
};

// Mirrors wipefs_alloc() (wipefs.cpp lines 252-335). Returns the errno
// wipefs_alloc would return. In simulate mode no fd is used.
static int probe_wipefs_alloc(const Options &o, int fd, size_t block_size, ProbeCtx *ctx)
{
	int err = 0;
	uint64_t numBlocks = 0;
	uint32_t nativeBlockSize = 0;
	off_t totalSizeInBytes = 0;
	class ExtentManager *extMan = NULL;
	struct stat sbuf = { 0 };
	char *diskname = NULL;

	if (o.simulate) {
		plog("ALLOC simulate: pretending fstat says S_IFCHR, DKIOCGETBLOCKSIZE=%u DKIOCGETBLOCKCOUNT=%llu",
		     o.simBlockSize, (unsigned long long)o.simBlockCount);
		nativeBlockSize = o.simBlockSize;
		numBlocks = o.simBlockCount;
		totalSizeInBytes = numBlocks * nativeBlockSize;
	} else {
		errno = 0;
		int fr = fstat(fd, &sbuf);
		int fe = errno;
		plog("ALLOC fstat ret=%d errno=%d st_mode=0%o (%s) st_size=%lld st_blksize=%d st_rdev=0x%x",
		     fr, fe, sbuf.st_mode,
		     S_ISCHR(sbuf.st_mode) ? "S_IFCHR" : S_ISBLK(sbuf.st_mode) ? "S_IFBLK" :
		     S_ISREG(sbuf.st_mode) ? "S_IFREG" : "other",
		     (long long)sbuf.st_size, sbuf.st_blksize, (unsigned)sbuf.st_rdev);
		switch (sbuf.st_mode & S_IFMT) {
		case S_IFCHR:
		case S_IFBLK: {
			errno = 0;
			int r1 = ioctl(fd, DKIOCGETBLOCKSIZE, (char *)&nativeBlockSize);
			int e1 = errno;
			plog("ALLOC ioctl(DKIOCGETBLOCKSIZE)  ret=%d errno=%d %s value=%u", r1, e1, errname(e1), nativeBlockSize);
			if (r1 < 0) { err = e1; goto labelExit; }
			errno = 0;
			int r2 = ioctl(fd, DKIOCGETBLOCKCOUNT, (char *)&numBlocks);
			int e2 = errno;
			plog("ALLOC ioctl(DKIOCGETBLOCKCOUNT) ret=%d errno=%d %s value=%llu", r2, e2, errname(e2),
			     (unsigned long long)numBlocks);
			if (r2 < 0) { err = e2; goto labelExit; }
			totalSizeInBytes = numBlocks * nativeBlockSize;

			diskname = query_disk_info(fd);
			break;
		}
		case S_IFREG:
			ctx->isRegularFile = true;
			// PROBE: wipefs does not call the DKIOC ioctls on a regular file;
			// the probe tries them anyway so the failure is visible.
			{
				IoctlResult a = get<uint32_t>(fd, DKIOCGETBLOCKSIZE);
				IoctlResult b = get<uint64_t>(fd, DKIOCGETBLOCKCOUNT);
				log_ioctl("DKIOCGETBLOCKSIZE (probe only, S_IFREG)", a, nullptr);
				log_ioctl("DKIOCGETBLOCKCOUNT (probe only, S_IFREG)", b, nullptr);
			}
			nativeBlockSize = sbuf.st_blksize;
			numBlocks = sbuf.st_size / sbuf.st_blksize;
			totalSizeInBytes = sbuf.st_size;
			plog("ALLOC S_IFREG path: nativeBlockSize=st_blksize=%u numBlocks=%llu totalSizeInBytes=st_size=%lld",
			     nativeBlockSize, (unsigned long long)numBlocks, (long long)totalSizeInBytes);
			break;
		default:
			// wipefs.cpp sets errno (not err) here and returns 0 with a NULL
			// handle; the installed binary does the same.
			plog("ALLOC unsupported file type: wipefs_alloc would return 0 with a NULL handle (errno=EINVAL)");
			return EINVAL;
		}
	}
	// PROBE: the log line storagekitd prints right before wipefs_alloc
	// (-[DMToolEraseDisk wipeDisk:shared:], macOS 27.0 storagekitd 1076.0.4).
	plog("STORAGEKITD-EQUIVALENT blockSize=%d blockCount=%lld totalBytes=%lld fd=%d",
	     (int)nativeBlockSize, (long long)numBlocks, (long long)totalSizeInBytes, fd);

	if (block_size == 0) {
		block_size = nativeBlockSize;
	}
	if (block_size == 0 || block_size > UINT32_MAX || totalSizeInBytes == 0) {
		// PROBE: block_size > UINT32_MAX is not in wipefs; Init's uint32_t cast
		// would make it 0 (or a wrong size) and divide by it.
		err = EINVAL;
		goto labelExit;
	}

	ctx->fd = fd;
	extMan = &ctx->extMan;

	extMan->Init((uint32_t)block_size, nativeBlockSize, totalSizeInBytes);
	gProbeCurrentFS = "FutureFS";    AddExtentsForFutureFS(extMan);
	gProbeCurrentFS = "HFS";         AddExtentsForHFS(extMan);
	gProbeCurrentFS = "MSDOS";       AddExtentsForMSDOS(extMan);
	gProbeCurrentFS = "NTFS";        AddExtentsForNTFS(extMan);
	gProbeCurrentFS = "UDF";         AddExtentsForUDF(extMan);
	gProbeCurrentFS = "UFS";         AddExtentsForUFS(extMan);
	gProbeCurrentFS = "ZFS";         AddExtentsForZFS(extMan);
	gProbeCurrentFS = "Partitions";  AddExtentsForPartitions(extMan);
	gProbeCurrentFS = "CoreStorage"; AddExtentsForCoreStorage(extMan);

	ctx->diskname = diskname;
	diskname = NULL;

	plog("ALLOC wipefs_alloc(fd, block_size=%zu) -> extMan.blockSize=%zu nativeBlockSize=%zu"
	     " totalBytes=%lld totalBlocks=%lld (howmany(totalBytes, blockSize))",
	     block_size, extMan->blockSize, extMan->nativeBlockSize,
	     (long long)extMan->totalBytes, (long long)extMan->totalBlocks);

labelExit:
	if (err != 0) {
		plog("ALLOC wipefs_alloc would FAIL: err=%d %s (%s)", err, errname(err), strerror(err));
		if (diskname != NULL) free(diskname);
	}
	return err;
}

static void print_extents(const ProbeCtx &ctx)
{
	const ExtentManager &m = ctx.extMan;
	plog("REQUESTS (in call order; block units of extMan.blockSize=%zu):", m.blockSize);
	for (const auto &r : gProbeRequests) {
		if (r.byteRange) {
			plog("  %-32s AddByteRangeExtent(byte %lld, len %lld) -> blocks [%lld, +%lld)",
			     r.fs.c_str(), (long long)r.addr, (long long)r.count,
			     (long long)r.blockAddr, (long long)r.numBlocks);
		} else {
			plog("  %-32s AddBlockRangeExtent(block %lld, count %lld)",
			     r.fs.c_str(), (long long)r.addr, (long long)r.count);
		}
	}
	plog("EXTENTS after merging (the list wipefs_wipe iterates, sentinels included):");
	int i = 0;
	for (const auto &e : m.extentList) {
		plog("  extent %d: blockAddr=%lld numBlocks=%lld  bytes [%lld, %lld)  len=%lld",
		     i++, (long long)e.blockAddr, (long long)e.numBlocks,
		     (long long)(e.blockAddr * (off_t)m.blockSize),
		     (long long)((e.blockAddr + e.numBlocks) * (off_t)m.blockSize),
		     (long long)(e.numBlocks * (off_t)m.blockSize));
	}
}

// ===========================================================================
// PROBE: wipefs_wipe reproduction
// ===========================================================================

struct WipeStats {
	int pwrites = 0;
	long long bytes = 0;
	int flaggedBeyondEnd = 0;
	int flaggedBeyondMedia = 0;
	long long expectedScsiWrites = 0;
	int lastPwriteErrno = 0;     // errno the last real pwrite itself set (0 = untouched)
	ssize_t lastPwriteRet = 0;
	bool countMismatch = false;  // a pwrite returned != requested (wipefs_wipe's check failed)
};

// The one I/O call of the loop. Dry run: record only. Write mode: errno=0,
// pwrite, log requested length, offset, return value, errno and the errno
// wipefs itself would have read.
static ssize_t probe_pwrite(bool doWrite, int fd, const void *buf, size_t len, off_t off,
                            int *wipefsErrnoShadow, int chunkIndex, int extentIndex,
                            size_t bufSize, off_t totalBytes, size_t nativeBlockSize,
                            uint64_t splitBytes, long long mediaSize, WipeStats *st)
{
	st->pwrites++;
	off_t end = off + (off_t)len;
	bool beyondEnd = end > totalBytes;
	bool beyondMedia = mediaSize >= 0 && end > mediaSize;
	if (beyondEnd) st->flaggedBeyondEnd++;
	if (beyondMedia) st->flaggedBeyondMedia++;
	long long scsi = splitBytes ? (long long)((len + splitBytes - 1) / splitBytes) : -1;
	if (scsi > 0) st->expectedScsiWrites += scsi;
	char split[96] = "";
	if (scsi > 0) snprintf(split, sizeof(split), " scsiCmds<=%lld(at %llu B)", scsi, (unsigned long long)splitBytes);

	if (!doWrite) {
		plog("PLAN pwrite #%d (extent %d chunk %d) offset=%lld len=%zu end=%lld LBA %lld..%lld %s%s%s%s",
		     st->pwrites, extentIndex, chunkIndex, (long long)off, len, (long long)end,
		     (long long)(off / (off_t)nativeBlockSize), (long long)((end - 1) / (off_t)nativeBlockSize),
		     len < bufSize ? "PARTIAL-CHUNK" : "full-chunk", split,
		     beyondEnd ? " !!BEYOND-totalBytes" : "", beyondMedia ? " !!BEYOND-IOMedia-Size" : "");
		st->bytes += (long long)len;
		return (ssize_t)len;   // a dry run assumes success
	}

	int stale = errno;          // what wipefs's `err = errno` reads if pwrite does not set errno
	plog("WRITE pwrite #%d (extent %d chunk %d) offset=%lld len=%zu end=%lld LBA %lld..%lld%s%s%s  [issuing]",
	     st->pwrites, extentIndex, chunkIndex, (long long)off, len, (long long)end,
	     (long long)(off / (off_t)nativeBlockSize), (long long)((end - 1) / (off_t)nativeBlockSize),
	     split, beyondEnd ? " !!BEYOND-totalBytes" : "", beyondMedia ? " !!BEYOND-IOMedia-Size" : "");
	errno = 0;
	uint64_t t0 = mach_absolute_time();
	ssize_t ret = pwrite(fd, buf, len, off);
	int e = errno;
	uint64_t t1 = mach_absolute_time();
	double ms = (double)(t1 - t0) * gTimebase.numer / gTimebase.denom / 1e6;
	const char *cls = ret == (ssize_t)len ? "OK" : ret < 0 ? "ERROR(-1)" : ret < (ssize_t)len ? "SHORT" : "OVERLONG(ret>len)";
	if (e != 0) *wipefsErrnoShadow = e;
	st->lastPwriteErrno = e;
	st->lastPwriteRet = ret;
	plog("WRITE pwrite #%d ret=%zd requested=%zu errno_after=%d %s (%s) errno_before_reset=%d %s %.3f ms => %s",
	     st->pwrites, ret, len, e, errname(e), strerror(e), stale, errname(stale), ms, cls);
	if (ret > 0) st->bytes += (long long)ret;
	errno = *wipefsErrnoShadow;   // restore the value wipefs would observe
	return ret;
}

// Returns the err wipefs_wipe would return. *countMismatch (optional) is set
// when a pwrite returned the wrong count, even if that err is 0.
static int probe_wipefs_wipe(const Options &o, ProbeCtx *handle, uint64_t splitBytes, long long mediaSize,
                             const ExtraLimits &lim, bool *countMismatch = nullptr)
{
	const bool doWrite = o.write;
	int err = 0;
	uint8_t *bufZero = NULL;
	ListExtIt curExt;
	size_t bufSize;
	dk_extent_t extent;
	dk_unmap_t unmap;
	WipeStats st;
	int wipefsErrnoShadow = 0;
	const char *shadowSource = "stale errno from before wipefs_wipe (unknown inside storagekitd)";

	if (handle->diskname != NULL) {
		// Remove this disk's entry from the xART.
		// PROBE: skipped. wipefs runs `/usr/sbin/xartutil --erase-disk <diskN>`
		// here; it changes xART state, needs root and does no I/O on this disk.
		plog("WIPE step 1: wipefs would run `/usr/sbin/xartutil --erase-disk %s` (probe SKIPS it)", handle->diskname);
	}

	memset(&extent, 0, sizeof(dk_extent_t));
	extent.length = handle->extMan.totalBytes;

	memset(&unmap, 0, sizeof(dk_unmap_t));
	unmap.extents = &extent;
	unmap.extentsCount = 1;
	//
	// Since we always issue a a single extent, set the kIOStorageUnmapOptionWhole
	// option flag for drive pre-conditioning.
	//
	unmap.options = kIOStorageUnmapOptionWhole;

	if (!doWrite) {
		plog("PLAN ioctl(DKIOCUNMAP, {offset=0 length=%llu options=kIOStorageUnmapOptionWhole}) (NOT sent in dry run;"
		     " return value ignored by wipefs; DK_FEATURE_UNMAP=%s)",
		     (unsigned long long)extent.length,
		     lim.haveFeatures ? ((lim.features & DK_FEATURE_UNMAP) ? "1" : "0 -> expect ENOTSUP") : "unknown");
	} else if (o.skipUnmap) {
		plog("WIPE step 2: DKIOCUNMAP SKIPPED (--skip-unmap, NOT faithful to wipefs)");
		wipefsErrnoShadow = errno;
		shadowSource = "stale errno left in this probe process before the (skipped) DKIOCUNMAP";
	} else {
		//
		// Don't bother to check the return value since this is mostly
		// informational for the lower-level drivers.
		//
		int before = errno;
		errno = 0;
		int r = ioctl(handle->fd, DKIOCUNMAP, (caddr_t)&unmap);
		int e = errno;
		plog("WIPE step 2: ioctl(DKIOCUNMAP, offset=0 length=%llu options=Whole) ret=%d errno=%d %s (%s)"
		     " [wipefs ignores this result]",
		     (unsigned long long)extent.length, r, e, errname(e), strerror(e));
		if (r == -1) {
			wipefsErrnoShadow = e;
			shadowSource = "STALE errno left by the ignored DKIOCUNMAP";
		} else {
			wipefsErrnoShadow = before;
		}
		errno = wipefsErrnoShadow;
	}

	bufSize = 128 * 1024; // issue large I/O to get better performance
	if (handle->extMan.nativeBlockSize > bufSize) {
	    bufSize = handle->extMan.nativeBlockSize;
	}
	bufZero = new uint8_t[bufSize];
	bzero(bufZero, bufSize);
	plog("WIPE bufSize=%zu buffer=%p (addr%%4096=%lu addr%%16384=%lu)", bufSize, (void *)bufZero,
	     (unsigned long)((uintptr_t)bufZero % 4096), (unsigned long)((uintptr_t)bufZero % 16384));

	off_t byteOffset, totalBytes;
	size_t numBytes, numBytesToWrite, blockSize;
	int extentIndex = -1;   // PROBE

	blockSize = handle->extMan.blockSize;
	totalBytes = handle->extMan.totalBytes;
	// write zero to all extents
	for (curExt = handle->extMan.extentList.begin(); curExt != handle->extMan.extentList.end(); curExt++) {
		extentIndex++;   // PROBE
		byteOffset = curExt->blockAddr * blockSize;
		numBytes = curExt->numBlocks * blockSize;
		off_t rawOffset = byteOffset; size_t rawBytes = numBytes;   // PROBE
		// make both offset and numBytes on native block boundary
		if (byteOffset % handle->extMan.nativeBlockSize != 0 ||
			numBytes % handle->extMan.nativeBlockSize != 0) {
			size_t nativeBlockSize = handle->extMan.nativeBlockSize;
			off_t newOffset, newEndOffset;
			newOffset = byteOffset / nativeBlockSize * nativeBlockSize;
			newEndOffset = wipefs_roundup(byteOffset + numBytes, nativeBlockSize);
			byteOffset = newOffset;
			numBytes = newEndOffset - newOffset;
		}
		off_t alignedOffset = byteOffset; size_t alignedBytes = numBytes;   // PROBE
		if (byteOffset + (off_t)numBytes > totalBytes) {
			numBytes = totalBytes - byteOffset;
		}
		// PROBE: describe the extent before writing it
		{
			size_t chunks = numBytes ? (numBytes + bufSize - 1) / bufSize : 0;
			size_t last = numBytes ? numBytes - (chunks - 1) * bufSize : 0;
			plog("%s extent %d: raw [%lld,+%zu) aligned [%lld,+%zu)%s -> write [%lld,+%zu) end=%lld"
			     " chunks=%zu finalChunk=%zu%s",
			     doWrite ? "WRITE" : "PLAN", extentIndex,
			     (long long)rawOffset, rawBytes, (long long)alignedOffset, alignedBytes,
			     (alignedOffset != rawOffset || alignedBytes != rawBytes) ? " (realigned)" : "",
			     (long long)byteOffset, numBytes, (long long)(byteOffset + (off_t)numBytes), chunks, last,
			     numBytes != alignedBytes ? " (CLIPPED at totalBytes)" : "");
		}
		int chunk = 0;   // PROBE
		while (numBytes > 0) {
			numBytesToWrite = min(numBytes, bufSize);
			// PROBE: the original line is
			//   if (pwrite(handle->fd, bufZero, numBytesToWrite, byteOffset) != (ssize_t)numBytesToWrite) {
			if (probe_pwrite(doWrite, handle->fd, bufZero, numBytesToWrite, byteOffset, &wipefsErrnoShadow,
			                 chunk++, extentIndex, bufSize, totalBytes, handle->extMan.nativeBlockSize,
			                 splitBytes, mediaSize, &st) != (ssize_t)numBytesToWrite) {
				err = errno;
				st.countMismatch = true;
				plog("WRITE MISMATCH: pwrite returned %zd for %zu requested; wipefs stops here (no retry,"
				     " no completion of the short write). wipefs_wipe would return err=%d %s (%s);"
				     " source of that errno: %s",
				     st.lastPwriteRet, numBytesToWrite, err, errname(err), strerror(err),
				     st.lastPwriteErrno != 0 ? "set by this pwrite itself"
				     : err == 0 ? "none: errno was 0, so wipefs would report SUCCESS despite the mismatch"
				     : shadowSource);
				goto labelExit;
			}
			numBytes -= numBytesToWrite;
			byteOffset += numBytesToWrite;
		}
	}

  labelExit:

	if (!doWrite) {
		plog("PLAN ioctl(DKIOCSYNCHRONIZECACHE) (NOT sent in dry run)");
		plog("PLAN summary: %d pwrites, %lld bytes, bufSize=%zu, %d beyond totalBytes, %d beyond IOMedia Size%s",
		     st.pwrites, st.bytes, bufSize, st.flaggedBeyondEnd, st.flaggedBeyondMedia,
		     mediaSize < 0 ? " (IOMedia Size unknown)" : "");
		if (splitBytes) {
			plog("PLAN expected SCSI WRITE commands if every pwrite is split at %llu bytes: %lld",
			     (unsigned long long)splitBytes, st.expectedScsiWrites);
		}
	} else {
		errno = 0;
		int r = ioctl(handle->fd, DKIOCSYNCHRONIZECACHE);
		int e = errno;
		plog("WIPE labelExit: ioctl(DKIOCSYNCHRONIZECACHE) ret=%d errno=%d %s [wipefs ignores this result]",
		     r, e, errname(e));
		plog("WIPE result: wipefs_wipe would return err=%d %s (%s); pwrites issued=%d bytes reported=%lld%s",
		     err, errname(err), strerror(err), st.pwrites, st.bytes,
		     !st.countMismatch ? ""
		     : err == 0 ? "; !! COUNT MISMATCH: wipefs_wipe FAILED its pwrite count check but errno was 0,"
		                  " so its return value reports success"
		     : "; !! COUNT MISMATCH: wipefs_wipe FAILED its pwrite count check");
	}
	if (countMismatch) *countMismatch = st.countMismatch;
	if (bufZero != NULL)
		delete[] bufZero;

	return err;
} // probe_wipefs_wipe

// What storagekitd does after wipefs_wipe fails (disassembly of
// -[DMToolEraseDisk wipeDisk:shared:], storagekitd 1076.0.4, macOS 27.0):
// pwrite(fd, zeros, blockSize, 0) -> "canWriteFirst=%d errno=%d", then
// pwrite(fd, zeros, blockSize, totalBytes - blockSize) -> "canWriteLast=...",
// where canWrite = (ret != -1) and errno is read only when ret == -1.
static void storagekitd_followup(int fd, uint32_t blockSize, off_t totalBytes)
{
	std::vector<uint8_t> buf(blockSize, 0);
	off_t offs[2] = { 0, totalBytes - (off_t)blockSize };
	const char *names[2] = { "canWriteFirst", "canWriteLast" };
	for (int i = 0; i < 2; i++) {
		errno = 0;
		ssize_t r = pwrite(fd, buf.data(), blockSize, offs[i]);
		int e = errno;
		plog("FOLLOWUP %s: pwrite(len=%u, offset=%lld) ret=%zd errno=%d %s -> storagekitd logs %s=%d errno=%d%s",
		     names[i], blockSize, (long long)offs[i], r, e, errname(e), names[i], r != -1 ? 1 : 0,
		     r == -1 ? e : 0, (r != -1 && r != (ssize_t)blockSize) ? "  !! ret != len (storagekitd does not check)" : "");
	}
}

// "disk7s2s1" -> "disk7"; "" when `name` is not diskN[sM...].
static std::string whole_disk_of(const char *name)
{
	if (strncmp(name, "disk", 4) != 0 || !isdigit((unsigned char)name[4])) return "";
	const char *q = name + 4;
	while (isdigit((unsigned char)*q)) q++;
	return std::string(name, (size_t)(q - name));
}

// True if an IOMedia whose BSD name is on whole disk `bsd` is an ancestor of
// the media `mountedBsd`. Catches APFS volumes (diskMsK on the synthesized
// container diskM whose physical store is diskNsJ), CoreStorage and any other
// layering: the store's IOMedia sits above the volume in the service plane.
static bool media_stacked_on(const char *mountedBsd, const std::string &bsd, std::string *via)
{
	CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault, 0, mountedBsd);
	io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, match);   // consumes match
	if (!media) return false;
	io_iterator_t it = MACH_PORT_NULL;
	bool hit = false;
	if (IORegistryEntryCreateIterator(media, kIOServicePlane,
	                                  kIORegistryIterateRecursively | kIORegistryIterateParents, &it) == KERN_SUCCESS) {
		io_registry_entry_t e;
		while (!hit && (e = IOIteratorNext(it)) != MACH_PORT_NULL) {
			CFTypeRef n = IORegistryEntryCreateCFProperty(e, CFSTR(kIOBSDNameKey), nullptr, 0);
			char name[128];
			if (n && CFGetTypeID(n) == CFStringGetTypeID() &&
			    CFStringGetCString((CFStringRef)n, name, sizeof(name), kCFStringEncodingUTF8) &&
			    whole_disk_of(name) == bsd) {
				hit = true;
				*via = name;
			}
			if (n) CFRelease(n);
			IOObjectRelease(e);
		}
		IOObjectRelease(it);
	}
	IOObjectRelease(media);
	return hit;
}

// Refuses to write to a disk that has a mounted slice, or that is the
// physical store of a mounted APFS (or otherwise layered) volume.
static bool disk_has_mounts(const std::string &bsd)
{
	struct statfs *mnts = nullptr;
	int n = getmntinfo(&mnts, MNT_NOWAIT);
	for (int i = 0; i < n; i++) {
		const char *from = mnts[i].f_mntfromname;
		if (strncmp(from, "/dev/", 5) != 0) continue;
		std::string mountedWhole = whole_disk_of(from + 5);
		if (mountedWhole.empty()) continue;
		if (mountedWhole == bsd) {
			plog("REFUSE %s is mounted at %s", from, mnts[i].f_mntonname);
			return true;
		}
		std::string via;
		if (media_stacked_on(from + 5, bsd, &via)) {
			plog("REFUSE %s (mounted at %s) is stacked on %s, which is on %s (APFS physical store or similar)",
			     from, mnts[i].f_mntonname, via.c_str(), bsd.c_str());
			return true;
		}
	}
	return false;
}

// ===========================================================================
// PROBE: --selftest, compare with the INSTALLED libutil on a regular file
// ===========================================================================

// Layout of libutil's private struct __wipefs_ctx (wipefs.cpp line 44):
// { int fd; ExtentManager extMan; char *diskname; }. The installed binary
// allocates 0x48 bytes for it and reads diskname at +0x40, fd at +0 and the
// ExtentManager fields at +0x08..+0x38 (dyld_info -disassemble, libutil 76.0).
struct InstalledCtxMirror {
	int fd;
	ExtentManager extMan;
	char *diskname;
};
static_assert(offsetof(InstalledCtxMirror, extMan) == 0x08, "layout");
static_assert(offsetof(InstalledCtxMirror, diskname) == 0x40, "layout");
static_assert(sizeof(InstalledCtxMirror) == 0x48, "layout");

static int selftest(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { plog("SELFTEST cannot open %s: %s", path, strerror(errno)); return 2; }
	struct stat sb;
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
		plog("SELFTEST refusing: %s is not a regular file (the installed wipefs_alloc is only ever called on regular files here)", path);
		close(fd);
		return 64;
	}
	plog("SELFTEST file %s st_size=%lld st_blksize=%d", path, (long long)sb.st_size, sb.st_blksize);
	int failures = 0;
	const size_t sizes[] = { 0, 512, 4096 };
	for (size_t bs : sizes) {
		void *h = nullptr;
		int r = wipefs_alloc(fd, bs, &h);   // INSTALLED libutil
		if (r != 0 || h == nullptr) {
			plog("SELFTEST block_size=%zu installed wipefs_alloc ret=%d handle=%p: cannot compare", bs, r, h);
			failures++;
			continue;
		}
		const InstalledCtxMirror *inst = reinterpret_cast<const InstalledCtxMirror *>(h);
		Options o;
		ProbeCtx mine;
		gProbeRequests.clear();
		gQuiet = true;
		int pr = probe_wipefs_alloc(o, fd, bs, &mine);
		gQuiet = false;
		bool same = pr == 0 && inst->fd == fd &&
		            inst->extMan.blockSize == mine.extMan.blockSize &&
		            inst->extMan.nativeBlockSize == mine.extMan.nativeBlockSize &&
		            inst->extMan.totalBytes == mine.extMan.totalBytes &&
		            inst->extMan.totalBlocks == mine.extMan.totalBlocks &&
		            inst->extMan.extentList.size() == mine.extMan.extentList.size();
		plog("SELFTEST block_size=%zu installed: blockSize=%zu native=%zu totalBytes=%lld totalBlocks=%lld extents=%zu",
		     bs, inst->extMan.blockSize, inst->extMan.nativeBlockSize, (long long)inst->extMan.totalBytes,
		     (long long)inst->extMan.totalBlocks, inst->extMan.extentList.size());
		plog("SELFTEST block_size=%zu probe:     blockSize=%zu native=%zu totalBytes=%lld totalBlocks=%lld extents=%zu",
		     bs, mine.extMan.blockSize, mine.extMan.nativeBlockSize, (long long)mine.extMan.totalBytes,
		     (long long)mine.extMan.totalBlocks, mine.extMan.extentList.size());
		auto a = inst->extMan.extentList.begin();
		auto b = mine.extMan.extentList.begin();
		for (int i = 0; a != inst->extMan.extentList.end() && b != mine.extMan.extentList.end(); ++a, ++b, ++i) {
			bool eq = a->blockAddr == b->blockAddr && a->numBlocks == b->numBlocks;
			if (!eq) same = false;
			plog("SELFTEST   extent %d installed [%lld,+%lld) probe [%lld,+%lld) %s", i,
			     (long long)a->blockAddr, (long long)a->numBlocks, (long long)b->blockAddr,
			     (long long)b->numBlocks, eq ? "same" : "!! DIFFERENT");
		}
		plog("SELFTEST block_size=%zu => %s", bs, same ? "PASS" : "FAIL");
		if (!same) failures++;
		wipefs_free(&h);   // INSTALLED libutil
	}
	close(fd);
	plog("SELFTEST %s", failures ? "FAILED" : "all PASS: the probe's extent computation matches the installed libutil");
	return failures ? 1 : 0;
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char **argv)
{
	mach_timebase_info(&gTimebase);
	gT0 = mach_absolute_time();

	Options o;
	bool dryRunFlag = false;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		auto need = [&](const char *opt) -> const char * {
			if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", opt); exit(64); }
			return argv[++i];
		};
		if (!strcmp(a, "--help") || !strcmp(a, "-h")) o.help = true;
		else if (!strcmp(a, "--provenance")) o.provenance = true;
		else if (!strcmp(a, "--selftest")) o.selftestPath = need(a);
		else if (!strcmp(a, "--allow-non-iscsikit")) o.allowNonIscsikit = true;
		else if (!strcmp(a, "--device")) o.device = need(a);
		else if (!strcmp(a, "--dry-run")) dryRunFlag = true;
		else if (!strcmp(a, "--write")) o.write = true;
		else if (!strcmp(a, "--i-understand-this-erases")) o.iUnderstand = true;
		else if (!strcmp(a, "--allow-large")) o.allowLarge = true;
		else if (!strcmp(a, "--storagekitd-followup")) o.storagekitdFollowup = true;
		else if (!strcmp(a, "--skip-unmap")) o.skipUnmap = true;
		else if (!strcmp(a, "--shlock")) o.shlock = true;
		else if (!strcmp(a, "--log")) o.logPath = need(a);
		else if (!strcmp(a, "--wipefs-block-size")) {
			uint64_t v;
			const char *arg = need(a);
			if (!parse_u64(arg, &v) || !valid_block_size(v)) {
				fprintf(stderr, "bad --wipefs-block-size %s: EINVAL (%s); expected a power of two in [512, 2^31]\n",
				        arg, strerror(EINVAL));
				return 64;
			}
			o.wipefsBlockSize = (size_t)v; o.haveWipefsBlockSize = true;
		} else if (!strcmp(a, "--split-bytes")) {
			if (!parse_u64(need(a), &o.splitBytes) || o.splitBytes == 0) {
				fprintf(stderr, "bad --split-bytes: EINVAL, expected a positive decimal or 0x hex number\n");
				return 64;
			}
		} else if (!strcmp(a, "--simulate")) {
			const char *v = need(a);
			const char *colon = strchr(v, ':');
			uint64_t bs = 0, cnt = 0;
			std::string left = colon ? std::string(v, (size_t)(colon - v)) : std::string();
			// cnt > 0 and cnt * bs must fit off_t (totalSizeInBytes)
			if (!colon || !parse_u64(left.c_str(), &bs) || !parse_u64(colon + 1, &cnt) ||
			    !valid_block_size(bs) || cnt == 0 || cnt > (uint64_t)INT64_MAX / bs) {
				fprintf(stderr, "bad --simulate: EINVAL, expected BLOCKSIZE:BLOCKCOUNT with BLOCKSIZE a power of two"
				        " in [512, 2^31] and BLOCKCOUNT > 0\n");
				return 64;
			}
			o.simulate = true; o.simBlockSize = (uint32_t)bs; o.simBlockCount = cnt;
		} else {
			fprintf(stderr, "unknown argument: %s\n\n", a);
			print_usage(stderr);
			return 64;
		}
	}

	// ---- argument validation -------------------------------------------
	if (o.help) { print_usage(stdout); return 0; }
	if ((o.selftestPath || o.provenance) && (o.device || o.simulate || o.write || (o.selftestPath && o.provenance))) {
		fprintf(stderr, "--selftest and --provenance each run alone (no --device, --simulate or --write)\n");
		return 64;
	}
	if (o.selftestPath && strncmp(o.selftestPath, "/dev/", 5) == 0) {
		fprintf(stderr, "refusing: --selftest takes a regular file\n");
		return 64;
	}
	if (o.write && dryRunFlag) { fprintf(stderr, "--write and --dry-run are mutually exclusive\n"); return 64; }
	if (o.simulate && (o.device || o.write)) { fprintf(stderr, "--simulate takes no --device and no --write\n"); return 64; }
	if (!o.simulate && !o.device && !o.selftestPath && !o.provenance) { print_usage(stderr); return 64; }
	if (o.write && !o.iUnderstand) {
		fprintf(stderr, "refusing: --write erases data and also needs --i-understand-this-erases\n");
		return 64;
	}
	if (!o.write && (o.iUnderstand || o.storagekitdFollowup || o.skipUnmap || o.shlock || o.allowNonIscsikit)) {
		fprintf(stderr, "note: --i-understand-this-erases/--storagekitd-followup/--skip-unmap/--shlock/"
		        "--allow-non-iscsikit only apply with --write\n");
	}
	std::string bsd;
	bool isDiskPath = false;
	if (o.device) {
		isDiskPath = bsd_name_from_path(o.device, true, &bsd);
		if (strncmp(o.device, "/dev/", 5) == 0 && !isDiskPath) {
			fprintf(stderr, "refusing: %s is not /dev/diskN or /dev/rdiskN\n", o.device);
			return 64;
		}
		if (o.write) {
			std::string whole;
#ifdef WIPEFS_PROBE_TEST_REGULAR_FILE_WRITES
			// Test builds only (never the default build): allow --write on a
			// regular file that is NOT under /dev, to exercise the write-mode
			// logging with a pwrite interposer.
			bool testFile = strncmp(o.device, "/dev/", 5) != 0;
#else
			bool testFile = false;
#endif
			if (!testFile &&
			    (!bsd_name_from_path(o.device, false, &whole) || strncmp(o.device, "/dev/rdisk", 10) != 0)) {
				fprintf(stderr, "refusing: --write needs a whole-disk raw device /dev/rdiskN (storagekitd opens /dev/r<disk>)\n");
				return 64;
			}
		}
	}
	if (o.logPath) {
		if (strncmp(o.logPath, "/dev/", 5) == 0) { fprintf(stderr, "refusing: --log must be a regular file\n"); return 64; }
		gLogFd = open(o.logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (gLogFd < 0) { fprintf(stderr, "cannot open log %s: %s\n", o.logPath, strerror(errno)); return 66; }
	}

	// ---- standalone modes (after --log is open) ------------------------
	if (o.provenance) { print_provenance(); print_installed_libutil(); return 0; }
	if (o.selftestPath) { print_installed_libutil(); return selftest(o.selftestPath); }

	plog("%s mode=%s device=%s", PROBE_VERSION,
	     o.simulate ? "SIMULATE" : o.write ? "WRITE (destructive)" : "DRY-RUN (read-only)",
	     o.device ? o.device : "(none)");
	print_provenance();
	print_installed_libutil();

	// ---- simulate: pure computation ----------------------------------
	if (o.simulate) {
		ProbeCtx ctx;
		int err = probe_wipefs_alloc(o, -1, o.haveWipefsBlockSize ? o.wipefsBlockSize : 0, &ctx);
		if (err) return 1;
		print_extents(ctx);
		ExtraLimits none;
		probe_wipefs_wipe(o, &ctx, o.splitBytes, -1, none);
		return 0;
	}

	// ---- registry facts (no open needed) ------------------------------
	RegistryFacts reg;
	if (isDiskPath) reg = dump_registry(bsd, "before");

	// ---- open ------------------------------------------------------------
	int flags = O_RDONLY;
	if (o.write) flags = o.shlock ? (O_RDWR | O_SHLOCK) : O_RDWR;
	errno = 0;
	int fd = open(o.device, flags);
	int oe = errno;
	plog("OPEN open(%s, %s) -> fd=%d errno=%d %s (%s)", o.device,
	     flags == O_RDONLY ? "O_RDONLY" : (flags & O_SHLOCK) ? "O_RDWR|O_SHLOCK" : "O_RDWR",
	     fd, oe, errname(oe), strerror(oe));
	if (fd < 0) {
		if (oe == EACCES || oe == EPERM) plog("OPEN hint: raw disk devices need root (sudo)");
		return 2;
	}

	ExtraLimits lim = extra_ioctls(fd);   // on a regular file these fail with ENOTTY

	ProbeCtx ctx;
	int err = probe_wipefs_alloc(o, fd, o.haveWipefsBlockSize ? o.wipefsBlockSize : 0, &ctx);
	if (err) { close(fd); return 1; }

	// ---- cross checks ------------------------------------------------------
	const off_t totalBytes = ctx.extMan.totalBytes;
	if (reg.found) {
		plog("CHECK IOMedia Size=%lld vs blockCount*blockSize=%lld: %s", reg.mediaSize, (long long)totalBytes,
		     reg.mediaSize == (long long)totalBytes ? "equal"
		     : reg.mediaSize > (long long)totalBytes ? "IOMedia larger (not a multiple of the block size?)"
		     : "!! IOMedia SMALLER than wipefs totalBytes");
		plog("CHECK IOMedia Preferred Block Size=%lld vs DKIOCGETBLOCKSIZE=%zu: %s", reg.preferredBlockSize,
		     ctx.extMan.nativeBlockSize,
		     reg.preferredBlockSize == (long long)ctx.extMan.nativeBlockSize ? "equal" : "!! differ");
	}
	if (totalBytes > kLargeDeviceLimit && !o.allowLarge) {
		plog("REFUSE device reports %lld bytes (> 64 GiB). This guard protects the 4 TB LUN."
		     " Re-run with --allow-large only if you are sure.", (long long)totalBytes);
		close(fd);
		return 3;
	}

	print_extents(ctx);

	uint64_t split = o.splitBytes ? o.splitBytes : lim.maxByteCountWrite;

	if (!o.write) {
		probe_wipefs_wipe(o, &ctx, split, reg.found ? reg.mediaSize : -1, lim);
		plog("DONE dry run: nothing was written; DKIOCUNMAP and DKIOCSYNCHRONIZECACHE were not sent.");
		close(fd);
		if (ctx.diskname) free(ctx.diskname);
		return 0;
	}

	// ---- write mode --------------------------------------------------------
#ifndef WIPEFS_PROBE_TEST_REGULAR_FILE_WRITES
	if (ctx.isRegularFile) { plog("REFUSE --write on a regular file"); close(fd); return 64; }
#else
	if (ctx.isRegularFile) plog("TEST BUILD: --write on a regular file allowed");
#endif
	if (isDiskPath && !reg.iscsikitAncestor) {
		if (!o.allowNonIscsikit) {
			plog("REFUSE %s has no iSCSIKit / IOUserSCSIParallelInterfaceController ancestor in the IORegistry;"
			     " it is not an iSCSIKit LUN. Re-run with --allow-non-iscsikit only if you are sure.", bsd.c_str());
			close(fd);
			return 6;
		}
		plog("WARNING %s is not under iSCSIKit; proceeding because of --allow-non-iscsikit", bsd.c_str());
	}
	if (!bsd.empty() && disk_has_mounts(bsd)) { plog("REFUSE %s has mounted volumes; unmount it first (diskutil unmountDisk)", bsd.c_str()); close(fd); return 4; }
	{
		IoctlResult w = get<uint32_t>(fd, DKIOCISWRITABLE);
		if (w.ret == 0 && w.value == 0) { plog("REFUSE DKIOCISWRITABLE says the media is not writable"); close(fd); return 5; }
	}
	plog("PLAN first (identical arithmetic, nothing written yet):");
	{
		Options dry = o; dry.write = false;
		probe_wipefs_wipe(dry, &ctx, split, reg.found ? reg.mediaSize : -1, lim);
	}
	plog("WRITE starting the real wipefs_wipe sequence on %s (%lld bytes)", o.device, (long long)totalBytes);
	bool mismatch = false;
	err = probe_wipefs_wipe(o, &ctx, split, reg.found ? reg.mediaSize : -1, lim, &mismatch);
	// A count mismatch is a failure even when errno (hence err) was 0: wipefs
	// would then return 0, but its own pwrite check had failed.
	const bool failed = err != 0 || mismatch;
	if (failed && o.storagekitdFollowup) {
		storagekitd_followup(fd, (uint32_t)ctx.extMan.nativeBlockSize, totalBytes);
	}
	close(fd);
	if (isDiskPath) (void)dump_registry(bsd, "after");
	plog("DONE write mode: wipefs_wipe would have returned %d %s%s", err, errname(err),
	     !mismatch ? ""
	     : err == 0 ? "; !! a pwrite COUNT MISMATCH occurred: wipefs_wipe FAILED its count check (errno was 0,"
	                  " so wipefs itself would have reported success)"
	     : "; a pwrite COUNT MISMATCH occurred");
	if (ctx.diskname) free(ctx.diskname);
	return err != 0 ? 10 : mismatch ? 11 : 0;
}

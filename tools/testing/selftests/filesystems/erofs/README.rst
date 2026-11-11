.. SPDX-License-Identifier: GPL-2.0

===========================
EROFS device FD integration
===========================

The opt-in device FD test covers FSCONFIG_SET_FD for external EROFS devices.
It needs root and a kernel with the options in ``config``.  The test does not
write to its metadata or blob devices.  Every mounted image has two external
blobs with different contents, so reads verify their order as well as FD use.

Build and run
=============

Build the binary and generate fixtures with erofs-utils.  The output directory
must be new::

    make headers
    make -C tools/testing/selftests/filesystems/erofs
    python3 tools/testing/selftests/filesystems/erofs/device_fd_fixtures.py \
        /path/to/mkfs.erofs /tmp/erofs-fd-fixtures

The generator creates 4 KiB and 16 KiB-block images.  Select the directory
matching the guest page size for block-backed DAX tests.  For regular-file
tests only, run inside a private mount namespace created by the program::

    sudo tools/testing/selftests/filesystems/erofs/erofs_device_fd \
        /tmp/erofs-fd-fixtures/4096

The regular-file suite reports 37 cases.  Its fixture directory must be
writable so separate checker FDs can obtain OFD write locks; the tests do
not write to the fixture contents.

To include block-backed tests, expose the matching metadata and two blobs as
three DAX-capable devices.  Pass them in metadata/blob-0/blob-1 order::

    sudo tools/testing/selftests/filesystems/erofs/erofs_device_fd \
        /tmp/erofs-fd-fixtures/4096 /dev/pmem0 /dev/pmem1 /dev/pmem2

The block-backed tests verify STATX_ATTR_DAX on the mounted files.  Loop
devices do not provide DAX.  No erofs-utils format change is needed; the
generator uses existing chunk/blob output and metadata rebuild support.

QEMU procedure
==============

Build a guest kernel with EROFS, file-backed mounts, FS_DAX, the appropriate
PMEM driver, devtmpfs, initramfs, serial console, SHMEM, TMPFS, MEMFD_CREATE,
FILE_LOCKING and BLK_DEV_LOOP built in.  Use CONFIG_ARM64_16K_PAGES=y and
CONFIG_OF_PMEM=y for arm64, or CONFIG_VIRTIO_PMEM=y for x86-64.  Use a static probe as ``/init`` and install
the selected fixture directory's ``meta.erofs``, ``bad-meta.erofs``,
``blob-0`` and ``blob-1`` under ``/images`` in the initramfs.
Create ``/dev``, ``/proc``, ``/sys`` and ``/tmp`` there, with a
``/dev/console`` character node (major 5, minor 1).
For example, compile the probe for arm64 with::

    aarch64-linux-musl-gcc -static -O2 -Wall -Wextra -Werror \
        -isystem /path/to/exported/arm64/kernel/headers/include \
        -Wl,-z,max-page-size=65536 \
        tools/testing/selftests/filesystems/erofs/erofs_device_fd.c -o init

Archive that initramfs using ``cpio --format=newc`` and gzip.  The generated
``*.pmem`` files contain the same images, padded to 128 MiB.  For x86-64,
append these devices to the QEMU command (Bash)::

    names=(meta.erofs blob-0 blob-1)
    pmem_args=()
    for i in 0 1 2; do
        pmem_args+=(-object "memory-backend-file,id=p$i,mem-path=$fixtures/${names[i]}.pmem,size=128M,share=on")
        pmem_args+=(-device "virtio-pmem-pci,memdev=p$i")
    done
    qemu-system-x86_64 -machine q35,accel=tcg -cpu max -smp 2 \
        -m 512M,slots=3,maxmem=2G -display none -serial stdio \
        -monitor none -no-reboot -nic none \
        -kernel bzImage -initrd initramfs.cpio.gz \
        -append 'console=ttyS0 panic=1 rdinit=/init loglevel=4' "${pmem_args[@]}"

Set ``fixtures`` to the selected absolute fixture directory.  On Apple Silicon
use ``qemu-system-aarch64 -machine virt,nvdimm=on,gic-version=3 -accel hvf
-cpu host`` instead, with ``Image`` and ``console=ttyAMA0``.  Replace each
virtio device argument with ``nvdimm,memdev=p$i,addr=...``, using addresses
0x100000000, 0x108000000 and 0x110000000.  The init program identifies the
three PMEM devices by fixture contents because firmware enumeration can
change their numbering.

Use headers exported from the tested kernel, including ``linux/loop.h``
with LOOP_CONFIGURE.  Every boot must report ``RESULT cases=79 failures=0``
and have no kernel warning, BUG, Oops or panic.
Compare each PMEM file's SHA-256 before and after the boot; all three must
be unchanged.  QEMU exit status alone does
not indicate success.  The init program reboots after reporting results;
``-no-reboot`` makes QEMU exit then.  These tests do not require replacing
the host's kernel, modules, initramfs or GRUB entries.

Development and coverage
========================

The implementation keeps the original open file until superblock creation.
Regular-file blobs reuse that file without resolving its name.  Block FDs
are reopened by device number with the filesystem holder and registered
against the superblock.  A separate claim flag distinguishes input FDs
retained by the context from block-device files registered for the
superblock, including failures before or partway through its creation.
Only registered files use fs_bdev_file_release(); input FDs use fput().
This change is independent of large-block support and applies to the
unmodified 7.4-rc2 base.

Coverage includes legacy mount, new-API paths, multiple FDs, both path/FD
orders, closing caller FDs before creation, unlinked files and block nodes,
memfds, preserving caller file positions, invalid descriptor types and modes,
wrong device counts, mismatched backing modes after a partial device open,
abandoned fs contexts and created-but-unmounted contexts.  Sixteen repeated
mount cycles per backing mode check teardown and subsequent device claims.
Content, EOF zeros and DAX flags are verified through pread and mmap.

All newly created mount contexts explicitly set ``ro`` before CMD_CREATE.
This permits a read-only primary device and matches the flags of an existing
read-only superblock when it is reused.

The additional 21 lifecycle cases check final file references as well as
holder claims.  Ordinary input files carry OFD read locks.  After closing
the caller's descriptors, an independently opened checker must still be
unable to obtain a write lock while EROFS owns the file, and must succeed
after the expected final fput().  Block tests create their own read-only
AUTOCLEAR loop devices and verify that the backing association disappears
after the last opener closes.  Releasing a holder claim alone is insufficient
to satisfy this check.  Deferred cleanup is allowed a bounded wait.

These cases include context abandonment, normal unmount, creation without
fsmount, incorrect device counts and types, failure after all blobs open,
the same FD passed twice, an exclusive input FD, superblock reuse and
reconfiguration.  The corrupt-root fixture has a valid superblock checksum
so that root validation fails after device initialization.  A live mapping
must keep its blobs alive across MNT_DETACH until munmap.  A separate valid
read-only primary loop device checks successful creation.  The tests do not
change the supplied PMEM devices' read-only flags.

Validation on 2026-11-11
=======================

The patched 7.4-rc2 kernels passed all four combinations below.  Each run
verified that all three PMEM backing files remained unchanged, with no
kernel warning, BUG, Oops or panic.

================== ============================= =============
Fixture build host Guest                         Result
================== ============================= =============
macOS arm64        x86-64, 4 KiB / virtio-pmem   79/79 passed
Linux x86-64       x86-64, 4 KiB / virtio-pmem   79/79 passed
macOS arm64        arm64, 16 KiB / NVDIMM        79/79 passed
Linux x86-64       arm64, 16 KiB / NVDIMM        79/79 passed
================== ============================= =============

Removing ``ro`` from the common context helper caused three cases to fail:
late root-validation failure on a read-only primary device, superblock reuse,
and successful creation on a read-only primary device.  The fixed probe
passed all 79 cases in each of the four combinations above (316 total).
A non-PID-1 invocation with a relative fixture path also passed all 37
regular-file cases inside its private mount namespace.
The initial test setup also exposed two harness requirements: MEMFD_CREATE
must be enabled explicitly, and PMEM devices must be identified by contents
instead of assumed firmware enumeration order.

The existing large-block/FSDAX regression was rerun using its retained full
probe and fixture sets: macOS/Linux images on both page sizes, 250 cases
per boot, all 1000 passed.  That regression includes 4/8/16/32/64 KiB blocks,
EOF zeros, poisoned tails, private COW, msync, inode_share and file-backed
mounts.  All four PMEM hashes were unchanged.  The test binary also built
through its selftest Makefile with -Wall -Wextra -Werror.

Real hardware PMEM, performance and older-kernel backports remain untested.
Passing an FD does not change the existing large-block/external-device
restrictions or make a regular-file blob DAX-capable.

.. SPDX-License-Identifier: GPL-2.0

==========================
EROFS large-block prototype
==========================

Scope and image selection
=========================

This prototype is based on Linux 7.4-rc2, commit
7475ea8b58616b33c04afb976cf0fbaad9dd63fb.  It allows uncompressed,
single-device EROFS images with filesystem blocks larger than PAGE_SIZE.
With CONFIG_TRANSPARENT_HUGEPAGE=y, the block layer permits blocks up to
64 KiB.  Without that option, BLK_MAX_BLOCK_SIZE remains PAGE_SIZE.
This change does not introduce a new on-disk format.

An image built on macOS with 16 KiB blocks can therefore be mounted by a
4 KiB-page Linux guest.  A 64 KiB-block image can be reused by the tested
4 KiB and 16 KiB-page guests, including FSDAX for plain files.  Specify the
block size explicitly when building a shared image; the formatter's default
still depends on the build host's page size.

For blocks larger than PAGE_SIZE, compressed data, metaboxes and external
device tables return EOPNOTSUPP.  Chunk-based files use buffered I/O in this
case.  Inline data uses buffered I/O as before.  The upstream restriction on
FSDAX for blocks smaller than PAGE_SIZE remains: a 4 KiB image mounts on a
16 KiB-page guest, but does not get FSDAX in this branch.  The earlier
sub-page-DAX prototype is a separate patch series.

Build and use
=============

Build the matching erofs-utils ``dax`` branch, based on
de2dbe430f066ab02d7272e6b47fcc70446bca61.  Its default compiled-in maximum
block size is 65536 on both macOS and Linux; an explicit configure-time
MAX_BLOCK_SIZE override is still supported::

    ./autogen.sh
    ./configure --disable-fuse
    make -j8
    mkfs/mkfs.erofs -b65536 -E^inline_data shared.erofs rootfs/
    fsck/fsck.erofs shared.erofs

Omitting compression and disabling inline data gives plain files the FSDAX
path.  Use ``-b16384`` if 16 KiB is the largest client page size.  No
``--dsunit`` option is needed when the filesystem block size itself meets
the page alignment requirement.

Enable CONFIG_EROFS_FS, CONFIG_FS_DAX and CONFIG_TRANSPARENT_HUGEPAGE in the
guest kernel, together with its PMEM driver.  For the tested QEMU devices,
these include CONFIG_BLK_DEV_PMEM, CONFIG_ZONE_DEVICE and either
CONFIG_VIRTIO_PMEM or CONFIG_OF_PMEM.  CONFIG_EROFS_FS_BACKED_BY_FILE enables
the buffered file-backed tests.  Rebuild and boot this branch's kernel.

Expose the image through a DAX-capable PMEM device, then mount it read-only::

    mkdir -p /mnt/erofs
    mount -t erofs -o ro,dax=always /dev/pmem0 /mnt/erofs

The device must actually support DAX.  A loop device backed by an image file
does not provide FSDAX.  The test probe below checks STATX_ATTR_DAX rather
than inferring DAX from a successful mount.  For buffered access to an image
file, use::

    mount -t erofs -o ro,dax=never shared.erofs /mnt/erofs

When embedding several images in one PMEM device, ``fsoffset`` must be
aligned to the image's block size.  Keep the underlying image immutable
while it is mounted, including through other aliases of the backing file.

End-to-end test procedure
========================

The selftests are opt-in because they boot a test kernel and need PMEM.
Generate fixtures using the patched tools, independently on each build OS::

    python3 tools/testing/selftests/filesystems/erofs_lbs.py \
        /path/to/erofs-utils/mkfs/mkfs.erofs \
        /path/to/erofs-utils/dump/dump.erofs /tmp/erofs-fixtures

The destination must be new.  The generator requires LZ4 support in mkfs for
the compression regression cases.  It creates ``fixtures.pmem`` (512 MiB),
``images.tar.gz``, ``cases`` and ``sha256.txt``.  Transfer the same outputs
unchanged to both guest hosts.  Independent builds may differ in filesystem
metadata such as symlink permissions inherited from the source OS.

Build the C probe statically for each guest architecture.  For example,
with a musl cross compiler::

    aarch64-linux-musl-gcc -static -O2 -Wall -Wextra -Werror \
        -Wl,-z,max-page-size=65536 \
        tools/testing/selftests/filesystems/erofs_dax.c -o /tmp/erofs-init

On Linux, assemble its initramfs as root (substitute the fixture directory)::

    mkdir -p /tmp/erofs-initrd/{dev,proc,sys,mnt}
    mknod -m 600 /tmp/erofs-initrd/dev/console c 5 1
    cp /tmp/erofs-init /tmp/erofs-initrd/init
    cp /tmp/erofs-fixtures/cases /tmp/erofs-initrd/cases
    tar -xzf /tmp/erofs-fixtures/images.tar.gz -C /tmp/erofs-initrd
    (cd /tmp/erofs-initrd && find . -print0 | \
        cpio --null -o --format=newc | gzip -1) > /tmp/erofs-initrd.cpio.gz

Compile all boot/test drivers into the guest kernel, including devtmpfs,
initramfs, ELF, procfs, sysfs, PCI, serial console, PMEM, EROFS and LZ4.
For x86-64 use 4 KiB pages and virtio-pmem::

    qemu-system-x86_64 -machine q35,accel=tcg -cpu max -smp 2 \
        -m 512M,slots=1,maxmem=2G -display none -serial stdio \
        -monitor none -no-reboot -nic none \
        -kernel /path/to/bzImage -initrd /tmp/erofs-initrd.cpio.gz \
        -append 'console=ttyS0 panic=1 rdinit=/init loglevel=4' \
        -object memory-backend-file,id=pmem,mem-path=/tmp/erofs-fixtures/fixtures.pmem,size=512M,share=on \
        -device virtio-pmem-pci,memdev=pmem

For arm64, build with CONFIG_ARM64_16K_PAGES=y and CONFIG_OF_PMEM=y.  On
an Apple Silicon Mac, run the arm64 kernel and arm64 initramfs with::

    qemu-system-aarch64 -machine virt,nvdimm=on,gic-version=3 \
        -accel hvf -cpu host -smp 2 -m 512M,slots=1,maxmem=2G \
        -display none -serial stdio -monitor none -no-reboot -nic none \
        -kernel /path/to/Image -initrd /tmp/erofs-initrd.cpio.gz \
        -append 'console=ttyAMA0 panic=1 rdinit=/init loglevel=4' \
        -object memory-backend-file,id=pmem,mem-path=/tmp/erofs-fixtures/fixtures.pmem,size=512M,share=on \
        -device nvdimm,memdev=pmem,addr=0x100000000

Each boot must report ``RESULT cases=N failures=0``, where N matches
the line count of ``cases``, with no kernel BUG, Oops, warning or panic.  Check the PMEM backing file's SHA-256 before and
after each boot; it must remain unchanged.  The init program reboots at
completion; ``-no-reboot`` makes QEMU exit without booting again.  QEMU exit
status alone does not indicate test success.

Coverage and limitations
========================

The fixtures cover 4/16/64 KiB blocks; plain, inline and chunk layouts;
file-backed mounts; inode_share; directory and xattr boundaries; pread and
mmap contents; EOF zeros; and SIGBUS beyond EOF.  Negative cases reject
nonzero plain/chunk tails, bad superblock checksums, unsupported block sizes,
invalid volume bounds and large-block compression.  Native-page LZ4 remains
covered.  Large xattrs use PAX tar headers to avoid host filesystem limits.

Build fixtures independently on macOS and Linux and reuse each set unchanged
on both guest page sizes.  Real hardware PMEM, performance and 64 KiB-page
guests remain unvalidated.

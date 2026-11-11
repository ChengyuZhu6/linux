// SPDX-License-Identifier: GPL-2.0
/* Root-only, opt-in test: FIXTURES [META_BDEV BLOB0_BDEV BLOB1_BDEV]. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <linux/reboot.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

#ifndef STATX_BASIC_STATS
#include <linux/stat.h>
#endif
#ifndef STATX_ATTR_DAX
#define STATX_ATTR_DAX 0x00200000
#endif

static char meta[4096], blobs[2][4096];
static const char *devices[3];
static unsigned int total, failures;

enum variant { NORMAL, UNLINKED, MEMFD, POSITION, ABORTED, DISCARDED, WRONG_TYPE, INVALID, LEGACY };

static int config(int fs, unsigned int cmd, const char *key, const void *value, int aux)
{
	return syscall(SYS_fsconfig, fs, cmd, key, value, aux);
}

static void copy_file(int out, const char *path)
{
	unsigned char buf[4096];
	int in = open(path, O_RDONLY);
	ssize_t n;

	assert(in >= 0);
	while ((n = read(in, buf, sizeof(buf))) > 0)
		assert(write(out, buf, n) == n);
	assert(!n);
	close(in);
}

static void check_content(bool dax)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t size = 65537, length = (size + page - 1) / page * page;

	for (int i = 0; i < 2; ++i) {
		char path[32];
		struct statx sx;
		unsigned char *data, buf[4096];
		int fd;

		snprintf(path, sizeof(path), "mnt/data-%d", i);
		fd = open(path, O_RDONLY);
		assert(fd >= 0);
		assert(!syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
		assert(sx.stx_size == size);
		assert(sx.stx_attributes_mask & STATX_ATTR_DAX);
		assert(!!(sx.stx_attributes & STATX_ATTR_DAX) == dax);
		for (size_t off = 0; off < size;) {
			size_t n = size - off;

			if (n > sizeof(buf))
				n = sizeof(buf);
			assert(pread(fd, buf, n, off) == (ssize_t)n);
			for (size_t j = 0; j < n; ++j)
				assert(buf[j] == ((off + j) * 17 + i) % 251);
			off += n;
		}
		assert(!pread(fd, buf, 1, size));
		data = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
		assert(data != MAP_FAILED);
		for (size_t j = 0; j < size; ++j)
			assert(data[j] == (j * 17 + i) % 251);
		for (size_t j = size; j < length; ++j)
			assert(!data[j]);
		assert(!munmap(data, length));
		close(fd);
	}
}

static void check_unclaimed(void)
{
	for (int i = 1; i <= 2; ++i) {
		int fd = open(devices[i], O_RDONLY | O_EXCL);

		assert(fd >= 0);
		close(fd);
	}
}

static void test_mount(bool block, unsigned int fdmask, enum variant variant, int count)
{
	int fs = syscall(SYS_fsopen, "erofs", FSOPEN_CLOEXEC);
	int aliases[2] = { -1, -1 }, fd, mnt;

	assert(fs >= 0);
	assert(!config(fs, FSCONFIG_SET_STRING, "dax", block ? "always" : "never", 0));
	if (block) {
		assert(!config(fs, FSCONFIG_SET_STRING, "source", devices[0], 0));
	} else {
		fd = open(meta, O_RDONLY);
		assert(fd >= 0);
		assert(!config(fs, FSCONFIG_SET_FD, "source", NULL, fd));
		close(fd);
	}
	for (int i = 0; i < count; ++i) {
		int n = i % 2;
		bool use_block = block ^ (variant == WRONG_TYPE && i == 1);
		const char *path = use_block ? devices[n + 1] : blobs[n];
		char tmp[32];

		if (!(fdmask & (1U << i))) {
			assert(!config(fs, FSCONFIG_SET_STRING, "device", path, 0));
			continue;
		}
		if (variant == MEMFD) {
			fd = syscall(SYS_memfd_create, "erofs-blob", 0);
			assert(fd >= 0);
			copy_file(fd, path);
		} else if (variant == UNLINKED) {
			snprintf(tmp, sizeof(tmp), "blob-%d", i);
			if (block) {
				struct stat st;

				assert(!stat(path, &st));
				assert(!mknod(tmp, S_IFBLK | 0600, st.st_rdev));
			} else {
				fd = open(tmp, O_CREAT | O_WRONLY | O_EXCL, 0600);
				assert(fd >= 0);
				copy_file(fd, path);
				close(fd);
			}
			fd = open(tmp, O_RDONLY);
			assert(fd >= 0);
			assert(!unlink(tmp));
		} else {
			fd = open(path, O_RDONLY);
			assert(fd >= 0);
		}
		if (variant == POSITION) {
			assert(lseek(fd, 123, SEEK_SET) == 123);
			aliases[n] = dup(fd);
			assert(aliases[n] >= 0);
		}
		assert(!config(fs, FSCONFIG_SET_FD, "device", NULL, fd));
		close(fd);
	}
	if (variant == ABORTED) {
		close(fs);
		goto out;
	}
	if (count != 2 || variant == WRONG_TYPE) {
		assert(config(fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0) == -1);
		assert(errno == EINVAL);
		close(fs);
		goto out;
	}
	assert(!config(fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0));
	if (variant == DISCARDED) {
		close(fs);
		goto out;
	}
	mnt = syscall(SYS_fsmount, fs, FSMOUNT_CLOEXEC, 0);
	assert(mnt >= 0);
	close(fs);
	assert(!syscall(SYS_move_mount, mnt, "", AT_FDCWD, "mnt", MOVE_MOUNT_F_EMPTY_PATH));
	close(mnt);
	check_content(block);
	assert(!umount("mnt"));
	for (int i = 0; i < 2; ++i) {
		if (aliases[i] >= 0) {
			assert(lseek(aliases[i], 0, SEEK_CUR) == 123);
			close(aliases[i]);
		}
	}
out:
	if (devices[0])
		check_unclaimed();
}

static void test_invalid_fds(void)
{
	int fds[6], pipefd[2];
	const int errors[] = { EBADF, EBADF, EINVAL, EINVAL, EINVAL, EBADF };

	fds[0] = open(blobs[0], O_WRONLY);
	fds[1] = open(blobs[0], O_PATH);
	fds[2] = open(".", O_RDONLY | O_DIRECTORY);
	fds[3] = open("/dev/null", O_RDONLY);
	assert(!pipe(pipefd));
	fds[4] = pipefd[0];
	fds[5] = 100000;
	for (size_t i = 0; i < ARRAY_SIZE(fds); ++i) {
		int fs = syscall(SYS_fsopen, "erofs", FSOPEN_CLOEXEC);

		assert(fs >= 0);
		assert(fds[i] >= 0);
		assert(config(fs, FSCONFIG_SET_FD, "device", NULL, fds[i]) == -1);
		assert(errno == errors[i]);
		close(fs);
		if (i != 5)
			close(fds[i]);
	}
	close(pipefd[1]);
}

static void test_legacy(bool block)
{
	char *options;

	assert(asprintf(&options, "device=%s,device=%s,dax=%s",
			block ? devices[1] : blobs[0], block ? devices[2] : blobs[1],
			block ? "always" : "never") >= 0);
	assert(!mount(block ? devices[0] : meta, "mnt", "erofs", MS_RDONLY, options));
	free(options);
	check_content(block);
	assert(!umount("mnt"));
	if (devices[0])
		check_unclaimed();
}

static void run_case(bool block, unsigned int mask, enum variant variant, int count)
{
	pid_t child = fork();
	int status;

	assert(child >= 0);
	++total;
	if (!child) {
		if (variant == INVALID)
			test_invalid_fds();
		else if (variant == LEGACY)
			test_legacy(block);
		else
			test_mount(block, mask, variant, count);
		_exit(0);
	}
	assert(waitpid(child, &status, 0) == child);
	umount2("mnt", MNT_DETACH);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		++failures;
	printf("%s block=%d fdmask=%u variant=%d count=%d\n",
	       WIFEXITED(status) && !WEXITSTATUS(status) ? "PASS" : "FAIL",
	       block, mask, variant, count);
}

static int suite(const char *fixtures)
{
	char scratch[] = "/tmp/erofs-device-fd-XXXXXX";
	char *base = realpath(fixtures, NULL);

	assert(base);
	assert(snprintf(meta, sizeof(meta), "%s/meta.erofs", base) < (int)sizeof(meta));
	for (int i = 0; i < 2; ++i)
		assert(snprintf(blobs[i], sizeof(blobs[i]), "%s/blob-%d", base, i) <
		       (int)sizeof(blobs[i]));
	free(base);
	assert(mkdtemp(scratch));
	assert(!chdir(scratch));
	assert(!mkdir("mnt", 0700));
	for (int block = 0; block <= !!devices[0]; ++block) {
		run_case(block, 0, LEGACY, 2);
		for (unsigned int mask = 0; mask < 4; ++mask)
			run_case(block, mask, NORMAL, 2);
		run_case(block, 3, UNLINKED, 2);
		run_case(block, 3, POSITION, 2);
		run_case(block, 3, ABORTED, 2);
		run_case(block, 3, DISCARDED, 2);
		run_case(block, 1, NORMAL, 1);
		run_case(block, 7, NORMAL, 3);
		if (devices[0])
			run_case(block, 3, WRONG_TYPE, 2);
		if (!block)
			run_case(block, 3, MEMFD, 2);
		for (int i = 0; i < 16; ++i)
			run_case(block, 3, NORMAL, 2);
	}
	run_case(false, 0, INVALID, 0);
	assert(!rmdir("mnt"));
	assert(!chdir("/"));
	assert(!rmdir(scratch));
	printf("RESULT cases=%u failures=%u\n", total, failures);
	return !!failures;
}

static void discover_devices(void)
{
	static const char * const paths[] = { "/dev/pmem0", "/dev/pmem1", "/dev/pmem2" };

	/* Firmware enumeration need not preserve QEMU's device order. */
	for (size_t i = 0; i < ARRAY_SIZE(paths); ++i) {
		unsigned char magic[4], data[4];
		int fd = open(paths[i], O_RDONLY), role;

		assert(fd >= 0);
		assert(pread(fd, magic, sizeof(magic), 1024) == sizeof(magic));
		assert(pread(fd, data, sizeof(data), 0) == sizeof(data));
		close(fd);
		if (!memcmp(magic, "\xe2\xe1\xf5\xe0", 4))
			role = 0;
		else {
			assert(data[0] <= 1 && data[1] == data[0] + 17);
			role = data[0] + 1;
		}
		assert(!devices[role]);
		devices[role] = paths[i];
		printf("DEVICE %s role=%d\n", paths[i], role);
	}
}

int main(int argc, char **argv)
{
	int result;

	setbuf(stdout, NULL);
	if (getpid() == 1) {
		mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
		mount("proc", "/proc", "proc", 0, NULL);
		mount("sysfs", "/sys", "sysfs", 0, NULL);
		mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
		discover_devices();
		result = suite("/images");
		sync();
		syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			LINUX_REBOOT_CMD_RESTART, NULL);
		return result;
	}
	if ((argc != 2 && argc != 5) || geteuid()) {
		fprintf(stderr,
			"Run as root: %s FIXTURES [META_BDEV BLOB0_BDEV BLOB1_BDEV]\n", argv[0]);
		return 4;
	}
	if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
		perror("private mount namespace");
		return 4;
	}
	if (argc == 5)
		for (int i = 0; i < 3; ++i)
			devices[i] = argv[i + 2];
	return suite(argv[1]);
}

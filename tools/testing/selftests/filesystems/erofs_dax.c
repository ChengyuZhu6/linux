// SPDX-License-Identifier: GPL-2.0
/*
 * Run with PATH SIZE PATTERN_SEED EXPECT_DAX.
 * As init, /cases contains DEVICE LABEL PATH SIZE SEED DAX_4K DAX_16K OPTIONS.
 * DAX is 0/1, or -2 for EUCLEAN when opening a corrupt inode.
 * Mount errors: -1 = EINVAL, -3 = EBADMSG, -4 = EOPNOTSUPP.
 * Seeds UINT_MAX and UINT_MAX-1 test directory blocks and long symlinks.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef STATX_BASIC_STATS
#include <linux/stat.h>
#endif
#ifndef STATX_ATTR_DAX
#define STATX_ATTR_DAX 0x00200000
#endif

static int probe_tree(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *de;
	unsigned int count = 0;
	char name[256];

	assert(dir);
	while ((de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		++count;
	}
	assert(count == 300);
	for (unsigned int i = 0; i < 300; ++i) {
		unsigned char data[4];
		int fd;

		snprintf(name, sizeof(name), "entry-%04u-", i);
		memset(name + strlen(name), 'n', 210);
		name[221] = 0;
		fd = openat(dirfd(dir), name, O_RDONLY);
		assert(fd >= 0);
		assert(read(fd, data, sizeof(data)) == sizeof(data));
		assert(((unsigned int)data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24) == i);
		close(fd);
	}
	closedir(dir);
	return 0;
}

static int probe(const char *path, size_t size, unsigned int seed, int want_dax)
{
	struct statx sx;
	long page = sysconf(_SC_PAGESIZE);
	size_t length = (size + page - 1) / page * page;
	unsigned char *data, *shared;
	int fd, status;
	pid_t child;

	if (seed == UINT_MAX)
		return probe_tree(path);
	if (seed == UINT_MAX - 1) {
		char link[1001];

		assert(readlink(path, link, sizeof(link)) == 1000);
		for (int i = 0; i < 1000; ++i)
			assert(link[i] == 'l');
		return 0;
	}
	fd = open(path, O_RDONLY);
	if (want_dax == -2) {
		assert(fd < 0 && errno == EUCLEAN);
		printf("CHECK %s rejected corrupt EOF padding with EUCLEAN\n", path);
		return 0;
	}
	assert(fd >= 0);
	assert(!syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
	printf("CHECK %s page=%ld size=%zu DAX=%d expected=%d\n", path,
	       page, size, !!(sx.stx_attributes & STATX_ATTR_DAX), want_dax);
	assert(sx.stx_attributes_mask & STATX_ATTR_DAX);
	assert(!!(sx.stx_attributes & STATX_ATTR_DAX) == want_dax);
	assert(sx.stx_size == size);
	if (strstr(path, "xattr-file")) {
		unsigned char value[8209];
		struct statfs fs;
		ssize_t expected;

		assert(!fstatfs(fd, &fs));
		expected = fs.f_bsize / 2 + 17;
		if (expected > (ssize_t)sizeof(value))
			expected = sizeof(value);
		assert(fgetxattr(fd, "user.boundary", value, sizeof(value)) == expected);
		for (ssize_t i = 0; i < expected; ++i)
			assert(value[i] == 'x');
	}
	data = malloc(size ?: 1);
	assert(data);
	assert(pread(fd, data, size, 0) == (ssize_t)size);
	for (size_t i = 0; i < size; ++i)
		assert(data[i] == (i * 17 + (i >> 8) + seed) % 251);
	assert(pread(fd, data, 1, size) == 0);
	if (size) {
		shared = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
		assert(shared != MAP_FAILED);
		for (size_t i = 0; i < size; ++i)
			assert(shared[i] == (i * 17 + (i >> 8) + seed) % 251);
		for (size_t i = size; i < length; ++i)
			assert(shared[i] == 0);
		assert(!munmap(shared, length));
	}
	child = fork();
	assert(child >= 0);
	if (!child) {
		/* Force the load so an out-of-range mapping must raise SIGBUS. */
		volatile unsigned char *p = mmap(NULL, length + page, PROT_READ,
						MAP_SHARED, fd, 0);

		if (p == MAP_FAILED)
			_exit(2);
		(void)p[length];
		_exit(3);
	}
	assert(waitpid(child, &status, 0) == child);
	assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS);
	free(data);
	close(fd);
	return 0;
}

static int guest_init(void)
{
	char dev[64], label[64], path[256], opts[64];
	unsigned long size;
	unsigned int seed;
	int dax4, dax16, want, total = 0, failures = 0, status, fields;
	long page = sysconf(_SC_PAGESIZE);
	FILE *cases;
	pid_t child;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	printf("TEST guest_pagesize=%ld\n", page);
	cases = fopen("/cases", "r");
	assert(cases);
	while ((fields = fscanf(cases, "%63s %63s %255s %lu %u %d %d %63s",
				dev, label, path, &size, &seed, &dax4, &dax16, opts)) == 8) {
		++total;
		want = page == 4096 ? dax4 : dax16;
		if (mount(dev, "/mnt", "erofs", MS_RDONLY, opts)) {
			if ((want == -1 && errno == EINVAL) ||
			    (want == -3 && errno == EBADMSG) ||
			    (want == -4 && errno == EOPNOTSUPP)) {
				printf("PASS %s rejected with errno=%d\n", label, errno);
				continue;
			}
			printf("FAIL mount %s: %s\n", label, strerror(errno));
			++failures;
			continue;
		}
		if (want == -1 || want == -3 || want == -4) {
			printf("FAIL %s unexpectedly mounted\n", label);
			++failures;
			umount("/mnt");
			continue;
		}
		child = fork();
		if (!child)
			_exit(probe(path, size, seed, want));
		if (child < 0 || waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status)) {
			printf("FAIL %s\n", label);
			++failures;
		} else {
			printf("PASS %s %s\n", label, want == -2 ?
				       "corrupt-padding-rejected" : "read+mmap+EOF-zero+SIGBUS");
		}
		if (umount("/mnt"))
			++failures;
	}
	if (fields != EOF || ferror(cases))
		++failures;
	fclose(cases);
	printf("RESULT cases=%d failures=%d\n", total, failures);
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART, NULL);
	return failures != 0;
}

int main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	if (getpid() == 1)
		return guest_init();
	if (argc != 5) {
		fprintf(stderr, "Usage: %s PATH SIZE PATTERN_SEED EXPECT_DAX\n", argv[0]);
		return 4;
	}
	return probe(argv[1], strtoul(argv[2], NULL, 0),
		     strtoul(argv[3], NULL, 0), atoi(argv[4]));
}

/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "/sys/kernel/debug/criu/"
#define LIMIT (16 * 1024 * 1024)
static pid_t children[4];

static void cleanup(void)
{
	int i;

	for (i = 0; i < 4; i++)
		if (children[i] > 0) {
			kill(children[i], SIGKILL);
			waitpid(children[i], NULL, 0);
		}
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "A1_CHECK: FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, " (errno=%d: %s)\n", errno, strerror(errno));
	va_end(ap);
	exit(1);
}

#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

static char *read_fd(int fd, size_t chunk)
{
	char *buf = malloc(LIMIT);
	size_t len = 0;
	ssize_t n;

	CHECK(buf, "allocate read buffer");
	while ((n = read(fd, buf + len, chunk)) > 0) {
		len += n;
		CHECK(len + chunk < LIMIT, "output limit");
	}
	CHECK(n == 0, "read debugfs");
	buf[len] = 0;
	return buf;
}

static char *read_path(const char *path)
{
	int fd = open(path, O_RDONLY);
	char *s;

	CHECK(fd >= 0, "open %s", path);
	s = read_fd(fd, 8192);
	close(fd);
	return s;
}

static unsigned long long field(const char *s, const char *key, int base)
{
	char pattern[80], *p, *end;
	unsigned long long n;

	snprintf(pattern, sizeof(pattern), "%s=", key);
	p = strstr(s, pattern);
	CHECK(p, "missing field %s", key);
	p += strlen(pattern);
	n = strtoull(p, &end, base);
	CHECK(end != p, "invalid field %s", key);
	return n;
}

static int write_target(const char *s)
{
	int fd = open(ROOT "target", O_WRONLY);
	ssize_t n;
	int saved;

	if (fd < 0)
		return -1;
	n = write(fd, s, strlen(s));
	saved = errno;
	close(fd);
	errno = saved;
	return n == (ssize_t)strlen(s) ? 0 : -1;
}

static void target(pid_t pid)
{
	char s[32];

	snprintf(s, sizeof(s), "%d\n", pid);
	CHECK(!write_target(s), "set target %d", pid);
}

static pid_t start(const char *program, char *line, size_t size, int slot)
{
	int p[2];
	pid_t pid;
	FILE *f;

	CHECK(!pipe(p), "pipe");
	pid = fork();
	CHECK(pid >= 0, "fork");
	if (!pid) {
		close(p[0]);
		dup2(p[1], STDOUT_FILENO);
		close(p[1]);
		execl(program, program, NULL);
		_exit(127);
	}
	children[slot] = pid;
	close(p[1]);
	f = fdopen(p[0], "r");
	CHECK(f && fgets(line, size, f), "fixture ready: %s", program);
	fclose(f);
	return pid;
}

struct mapping {
	unsigned long start, end, offset, ino;
	unsigned int major, minor;
	char perms[5];
	char path[4096];
};

static int next_map(char **cursor, struct mapping *m)
{
	char *line, *nl;
	int off;

	while (**cursor) {
		line = *cursor;
		nl = strchr(line, '\n');
		CHECK(nl, "unterminated maps line");
		*nl = 0;
		*cursor = nl + 1;
		if (line[0] == '#')
			continue;
		memset(m, 0, sizeof(*m));
		CHECK(sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %n", &m->start,
			     &m->end, m->perms, &m->offset, &m->major, &m->minor,
			     &m->ino, &off) == 7, "bad map: %s", line);
		snprintf(m->path, sizeof(m->path), "%s", line[off] ? line + off : "-");
		return 1;
	}
	return 0;
}

static unsigned long compare_maps(pid_t pid)
{
	char name[80], *a, *b, *ac, *bc;
	struct mapping x, y;
	unsigned long count = 0;
	int nx, ny;

	snprintf(name, sizeof(name), "/proc/%d/maps", pid);
	a = ac = read_path(name);
	b = bc = read_path(ROOT "maps");
	for (;;) {
		nx = next_map(&ac, &x);
		ny = next_map(&bc, &y);
		CHECK(nx == ny, "maps length mismatch");
		if (!nx)
			break;
		CHECK(!memcmp(&x, &y, sizeof(x)),
		      "maps mismatch at %lx: %lx-%lx %s %lx %x:%x %lu '%s' vs "
		      "%lx-%lx %s %lx %x:%x %lu '%s'", x.start, x.start, x.end,
		      x.perms, x.offset, x.major, x.minor, x.ino, x.path, y.start,
		      y.end, y.perms, y.offset, y.major, y.minor, y.ino, y.path);
		count++;
	}
	free(a);
	free(b);
	return count;
}

static void expect_vma(const char *ext, unsigned long addr, const char *class,
		       const char *extra)
{
	char *copy = strdup(ext), *line, *save;
	int found = 0;
	char wanted[64];

	snprintf(wanted, sizeof(wanted), "class=%s ", class);
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '#')
			continue;
		if (addr >= field(line, "start", 16) && addr < field(line, "end", 16)) {
			CHECK(strstr(line, wanted), "class at %lx: %s", addr, line);
			CHECK(!extra || strstr(line, extra), "attribute at %lx: %s", addr, line);
			found = 1;
			break;
		}
	}
	free(copy);
	CHECK(found, "missing VMA at %lx", addr);
}

static void layout_test(void)
{
	char line[2048], *ext, *summary;
	pid_t pid = start("./tests/progs/a1-layout", line, sizeof(line), 0);
	unsigned long count;

	target(pid);
	count = compare_maps(pid);
	ext = read_path(ROOT "vmas_ext");
	summary = read_path(ROOT "task");
	CHECK(field(summary, "pid", 10) == (unsigned int)pid, "summary PID");
	CHECK(field(summary, "vma_count", 10) == count, "VMA count");
	CHECK(field(summary, "generation", 10) == field(ext, "generation", 10), "generation");
	expect_vma(ext, field(line, "anon", 16), "ANON_PRIVATE", "sample=0xa5");
	expect_vma(ext, field(line, "shared", 16), "ANON_SHARED", "sample=0x5a");
	expect_vma(ext, field(line, "file", 16), "FILE_SHARED", "sample=0x3c");
	expect_vma(ext, field(line, "private", 16), "FILE_PRIVATE", "sample=0x6d");
	expect_vma(ext, field(line, "memfd", 16), "FILE_SHARED", "sample=0x7e");
	expect_vma(ext, field(line, "ro", 16), "FILE_SHARED", "shared=1");
	expect_vma(ext, field(line, "guard", 16), "ANON_PRIVATE", "sample_status=SKIPPED");
	expect_vma(ext, field(line, "dont", 16), "ANON_PRIVATE", "dontdump=1");
	expect_vma(ext, field(line, "split", 16) + field(line, "page", 10),
		   "ANON_PRIVATE", "prot=r--");
	CHECK(strstr(ext, "special=VDSO") && strstr(ext, "special=VVAR"), "vDSO/vvar");
	CHECK(strstr(ext, " (deleted)"), "deleted file path");
	free(ext);
	free(summary);
	puts("A1_MAPS: PASS (fields, paths, classes, samples, special mappings)");
}

static void expect_errno(const char *value, int expected)
{
	errno = 0;
	CHECK(write_target(value) < 0 && errno == expected, "target '%s' expected errno %d", value, expected);
}

static void lifecycle_test(void)
{
	char line[2048], *old, *again, *s;
	pid_t a = start("./tests/progs/a1-layout", line, sizeof(line), 0);
	pid_t b = start("./tests/progs/a1-layout", line, sizeof(line), 1);
	unsigned long long generation;
	int fd;

	target(a);
	s = read_path(ROOT "target");
	generation = field(s, "generation", 10);
	free(s);
	fd = open(ROOT "task", O_RDONLY);
	CHECK(fd >= 0, "open task A");
	target(b);
	old = read_fd(fd, 7);
	CHECK(field(old, "pid", 10) == (unsigned int)a, "open binding");
	CHECK(field(old, "generation", 10) == generation, "captured generation");
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek");
	again = read_fd(fd, 19);
	CHECK(!strcmp(old, again), "repeat read must preserve snapshot");
	free(old);
	free(again);
	close(fd);
	expect_errno("2147483647", ESRCH);
	expect_errno("0", EINVAL);
	expect_errno("-1", EINVAL);
	expect_errno("1x", EINVAL);
	s = read_path(ROOT "target");
	CHECK(field(s, "pid", 10) == (unsigned int)b, "failed write preserves target");
	CHECK(field(s, "generation", 10) == generation + 1, "generation increment");
	free(s);
	target(2);
	fd = open(ROOT "task", O_RDONLY);
	CHECK(fd < 0 && errno == ESRCH, "kernel thread ESRCH");
	target(b);
	kill(b, SIGKILL);
	waitpid(b, NULL, 0);
	children[1] = 0;
	fd = open(ROOT "maps", O_RDONLY);
	CHECK(fd < 0 && errno == ESRCH, "dead target ESRCH");
	puts("A1_TARGET: PASS (binding, short reads, seek, errors, exit)");
}

static void permissions_test(void)
{
	pid_t pid = fork();
	int status;

	CHECK(pid >= 0, "fork permission test");
	if (!pid) {
		struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
		struct __user_cap_data_struct d[2] = { 0 };
		const char *names[] = { "target", "task", "maps", "vmas_ext", "status" };
		char path[128];
		size_t i;
		int fd = open(ROOT "task", O_RDONLY);
		char c;

		if (fd < 0 || syscall(SYS_capset, &h, d))
			_exit(2);
		if (read(fd, &c, 1) != -1 || errno != EPERM)
			_exit(3);
		close(fd);
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			snprintf(path, sizeof(path), ROOT "%s", names[i]);
			if (open(path, O_RDONLY) >= 0 || errno != EPERM)
				_exit(4);
		}
		if (!write_target("1") || errno != EPERM)
			_exit(5);
		_exit(0);
	}
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && !WEXITSTATUS(status), "capability checks: status=%d", status);
}

static void errors_test(void)
{
	char line[2048], *s;
	pid_t pid = start("./tests/progs/many-vmas", line, sizeof(line), 0);
	int i, fd;

	target(pid);
	CHECK(compare_maps(pid) >= 4000, "large VMA walk");
	permissions_test();
	for (i = 0; i < 100; i++) {
		pid = fork();
		CHECK(pid >= 0, "race fork");
		if (!pid)
			for (;;)
				pause();
		children[1] = pid;
		target(pid);
		fd = open(ROOT "maps", O_RDONLY);
		CHECK(fd >= 0, "race open");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		children[1] = 0;
		s = read_fd(fd, 113);
		free(s);
		close(fd);
		fd = open(ROOT "maps", O_RDONLY);
		CHECK(fd < 0 && errno == ESRCH, "race stale target");
	}
	puts("A1_ERRORS: PASS (4000+ VMAs, capabilities, 100 exit iterations)");
}

int main(int argc, char **argv)
{
	atexit(cleanup);
	CHECK(argc == 2, "usage: a1-check maps|target|errors");
	if (!strcmp(argv[1], "maps"))
		layout_test();
	else if (!strcmp(argv[1], "target"))
		lifecycle_test();
	else if (!strcmp(argv[1], "errors"))
		errors_test();
	else
		fail("unknown test");
	return 0;
}

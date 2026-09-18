#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "a5-fixture.h"

static int external_files(void *unused)
{
	(void)unused;
	for (;;) pause();
	return 0;
}

int main(int argc, char **argv)
{
	int s[2], fd, option = 1;
	const char *mode;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	fixture_init();
	if (argc == 4 && !strcmp(argv[1], "--dump")) {
		char request[1024];
		int len = snprintf(request, sizeof(request), "%s %s\n", argv[2], argv[3]);
		fd = open("/sys/kernel/debug/criu/dump", O_WRONLY);
		if (fd < 0) return 2;
		errno = 0;
		if (write(fd, request, len) != -1 || errno != EOPNOTSUPP) {
			fprintf(stderr, "expected EOPNOTSUPP, errno=%d\n", errno);
			close(fd);
			return 3;
		}
		close(fd);
		return 0;
	}
	if (argc != 3) return 1;
	mode = argv[1];
	if (!strcmp(mode, "clone-files") || !strcmp(mode, "thread-files")) {
		char *stack = malloc(65536);
		int flags = !strcmp(mode, "clone-files") ? CLONE_FILES | SIGCHLD :
			CLONE_VM | CLONE_SIGHAND | CLONE_THREAD;
		if (!stack || pipe(s) ||
		    clone(external_files, stack + 65536, flags, NULL) < 0)
			return 2;
	} else if (!strcmp(mode, "external-pipe") || !strcmp(mode, "shared-pipe")) {
		pid_t child;
		if (pipe(s)) return 2;
		child = fork();
		if (child < 0) return 2;
		if (!child) { close(s[0]); for (;;) pause(); }
		if (!strcmp(mode, "external-pipe")) close(s[1]);
	} else if (!strcmp(mode, "fown") || !strcmp(mode, "deleted")) {
		fd = open(argv[2], O_RDWR | O_CREAT, 0600);
		if (fd < 0) return 2;
		if (!strcmp(mode, "fown")) {
			if (fcntl(fd, F_SETOWN, getpid())) return 2;
		} else if (unlink(argv[2])) return 2;
	} else if (!strcmp(mode, "fifo")) {
		if (mkfifo(argv[2], 0600) || open(argv[2], O_RDWR | O_NONBLOCK) < 0)
			return 2;
	} else if (!strcmp(mode, "lock")) {
		struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
		fd = open(argv[2], O_RDWR | O_CREAT, 0600);
		if (fd < 0 || fcntl(fd, F_SETLK, &lock)) return 2;
	} else if (!strcmp(mode, "packet-pipe")) {
		if (pipe2(s, O_DIRECT) || write(s[1], "x", 1) != 1) return 2;
	} else if (!strcmp(mode, "listener") || !strcmp(mode, "pathname")) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) return 2;
		if (!strcmp(mode, "listener")) {
			address.sun_path[0] = 0;
			snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
				 "a5-listener-%d", getpid());
		} else {
			snprintf(address.sun_path, sizeof(address.sun_path), "%s", argv[2]);
		}
		if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
		    listen(fd, 1)) return 2;
		if (!strcmp(mode, "pathname")) {
			s[0] = socket(AF_UNIX, SOCK_STREAM, 0);
			if (s[0] < 0 || connect(s[0], (struct sockaddr *)&address, sizeof(address)))
				return 2;
			s[1] = accept(fd, NULL, NULL);
			if (s[1] < 0) return 2;
			close(fd); /* Reject connected pathname endpoints, not just listen. */
		}
	} else {
		int type = !strcmp(mode, "dgram") ? SOCK_DGRAM :
			   !strcmp(mode, "seqpacket") ? SOCK_SEQPACKET : SOCK_STREAM;
		if (socketpair(AF_UNIX, type, 0, s)) return 2;
		if (!strcmp(mode, "external")) {
			/* A live external peer lives in a child outside the dump set. */
			pid_t child = fork();
			if (child < 0) return 2;
			if (!child) { close(s[0]); for (;;) pause(); }
			close(s[1]);
		} else if (!strcmp(mode, "rights") || !strcmp(mode, "credentials")) {
			union {
				struct cmsghdr align;
				char data[CMSG_SPACE(sizeof(struct ucred))];
			} control;
			char byte = 'x';
			struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
			struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
				.msg_control = control.data };
			struct cmsghdr *cmsg;
			memset(&control, 0, sizeof(control));
			msg.msg_controllen = !strcmp(mode, "rights") ?
				CMSG_SPACE(sizeof(int)) : CMSG_SPACE(sizeof(struct ucred));
			cmsg = CMSG_FIRSTHDR(&msg);
			cmsg->cmsg_level = SOL_SOCKET;
			if (!strcmp(mode, "rights")) {
				fd = STDIN_FILENO;
				cmsg->cmsg_type = SCM_RIGHTS;
				cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
				memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
			} else {
				struct ucred cred = { .pid = getpid(), .uid = getuid(), .gid = getgid() };
				cmsg->cmsg_type = SCM_CREDENTIALS;
				cmsg->cmsg_len = CMSG_LEN(sizeof(cred));
				memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));
			}
			if (sendmsg(s[0], &msg, 0) != 1) return 2;
		} else if (!strcmp(mode, "passcred")) {
			if (setsockopt(s[1], SOL_SOCKET, SO_PASSCRED, &option, sizeof(option)))
				return 2;
		}
	}
	printf("pid=%d case=%s\n", getpid(), mode);
	fflush(stdout);
	fixture_alive();
}

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

static const struct option longopts[] = {
	{"format", required_argument, NULL, 'f'},
	{"count", required_argument, NULL, 'c'},
	{"mode", required_argument, NULL, 'm'},
	{NULL, 0, NULL, 0}
};
static const char *shortopts = "f:c:m:";

static volatile int terminate = 0;

static void sigterminate(int sig)
{
	(void)sig;
	terminate = 1;
}

int main(int argc, char **argv)
{
	const char *format = "pipe%d";
	int count = 10;
	mode_t mode = 0644;
	int c, ret;
	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch (c) {
		case 'f':
			format = optarg;
			break;
		case 'c':
			sscanf(optarg, "%d", &count);
			break;
		case 'm':
			sscanf(optarg, "%o", &mode);
			break;
		default:
			return 1;
		}
	}
	if (count < 1) {
		fprintf(stderr, "invalid count: %d\n", count);
		return 1;
	}
	umask(0);
	char path[PATH_MAX];
	int pipei;
	int fds[count];

	// Set up a signal handler to gracefully handle SIGINT and SIGTERM
	struct sigaction act = {
		.sa_handler = sigterminate
	};
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGINT);
	sigaddset(&act.sa_mask, SIGTERM);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);

	// If any iteration of this loop fails, the FIFOs will be unlinked
	// according to pipei - the number of FIFOs already successfully
	// created
	for (pipei = 0; pipei < count; pipei++) {
		snprintf(path, PATH_MAX, format, pipei);
		if (mkfifo(path, mode)) {
			perror("mkfifo");
			ret = 2;
			goto mkfifo_fail;
		}
		if ((fds[pipei] = open(path, O_RDONLY|O_NONBLOCK)) == -1) {
			perror("open");
			unlink(path);
			ret = 2;
			goto mkfifo_fail;
		}
	}

	int efd = epoll_create1(0);
	if (efd == -1) {
		perror("epoll_create1");
		ret = 3;
		goto mkfifo_fail;
	}
	struct epoll_event event = {
		.events = EPOLLIN
	};
	for (int i = 0; i < count; i++) {
		event.data.u32 = i;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, fds[i], &event)) {
			ret = 3;
			goto epoll_fail;
		}
	}

#define NUM_EVENTS 16
#define BUF_LEN 256
	struct epoll_event events[NUM_EVENTS];
	static char buf[BUF_LEN + 1];
	ssize_t bufread;
	int r;
	while (!terminate) {
		r = epoll_wait(efd, events, NUM_EVENTS, -1);
		if (r == -1) {
			if (errno = EINTR)
				continue;
			perror("epoll_wait");
			ret = 3;
			goto epoll_fail;
		}
		for (int i = 0; i < r; i++) {
			if (events[i].events & EPOLLIN) {
				snprintf(path, PATH_MAX, format, events[i].data.u32);
				bufread = read(fds[events[i].data.u32], buf, BUF_LEN);
				if (bufread == -1) {
					perror("open");
					ret = 4;
					goto epoll_fail;
				}
				buf[bufread] = '\0';
				printf("%s: [%s]\n", path, buf);
			}
		}
	}

	ret = 0;
epoll_fail:
	close(efd);
mkfifo_fail:
	for (int i = 0; i < pipei; i++) {
		snprintf(path, PATH_MAX, format, i);
		close(fds[i]);
		unlink(path);
	}
	return ret;
}

/* log_monitor.c - New thread to monitor logcat
 *
 * A universal logcat monitor for many usages. Add listeners to the list,
 * and the pointer of the new log line will be sent through pipes to trigger
 * asynchronous events without polling
 */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "magisk.h"
#include "utils.h"
#include "daemon.h"

int loggable = 1;
static int sockfd;
static pthread_t thread = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

enum {
	HIDE_EVENT,
	LOG_EVENT,
	DEBUG_EVENT
};

struct log_listener {
	int fd;
	int (*filter) (const char*);
};

static int am_proc_start_filter(const char *log) {
	return strstr(log, "am_proc_start") != NULL;
}

static int magisk_log_filter(const char *log) {
	char *ss;
	return (ss = strstr(log, " Magisk")) && (ss[-1] != 'D') && (ss[-1] != 'V');
}

static int magisk_debug_log_filter(const char *log) {
	return strstr(log, "am_proc_start") == NULL;
}

static struct log_listener log_events[] = {
	{	/* HIDE_EVENT */
		.fd = -1,
		.filter = am_proc_start_filter
	},
	{	/* LOG_EVENT */
		.fd = -1,
		.filter = magisk_log_filter
	},
	{	/* DEBUG_EVENT */
		.fd = -1,
		.filter = magisk_debug_log_filter
	}
};
#define EVENT_NUM (sizeof(log_events) / sizeof(struct log_listener))

static void test_logcat() {
	int log_fd = -1, log_pid;
	char buf[1];
	log_pid = exec_command(0, &log_fd, NULL, "logcat", NULL);
	if (read(log_fd, buf, sizeof(buf)) != sizeof(buf)) {
		loggable = 0;
		LOGD("log_monitor: cannot read from logcat, disable logging");
	}
	kill(log_pid, SIGTERM);
	waitpid(log_pid, NULL, 0);
}

static void sigpipe_handler(int sig) {
	close(log_events[HIDE_EVENT].fd);
	log_events[HIDE_EVENT].fd = -1;
}

static void *socket_thread(void *args) {
	/* This would block, so separate thread */
	while(1) {
		int fd = accept4(sockfd, NULL, NULL, SOCK_CLOEXEC);
		switch(read_int(fd)) {
			case HIDE_CONNECT:
				pthread_mutex_lock(&lock);
				log_events[HIDE_EVENT].fd = fd;
				pthread_mutex_unlock(&lock);
				thread = -1;
				return NULL;
			default:
				close(fd);
				break;
		}
	}
}

void log_daemon() {
	setsid();
	strcpy(argv0, "magisklogd");

	struct sockaddr_un sun;
	sockfd = setup_socket(&sun, LOG_DAEMON);
	if (xbind(sockfd, (struct sockaddr*) &sun, sizeof(sun)))
		exit(1);
	xlisten(sockfd, 1);
	LOGI("Magisk v" xstr(MAGISK_VERSION) "(" xstr(MAGISK_VER_CODE) ") logger started\n");

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = sigpipe_handler;
	sigaction(SIGPIPE, &act, NULL);

	// Setup log dumps
	rename(LOGFILE, LOGFILE ".bak");
	log_events[LOG_EVENT].fd = xopen(LOGFILE, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
#ifdef MAGISK_DEBUG
	log_events[DEBUG_EVENT].fd = xopen(DEBUG_LOG, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
#endif

	int log_fd = -1, log_pid;
	char line[PIPE_BUF];

	while (1) {
		if (!loggable) {
			// Disable all services
			for (int i = 0; i < EVENT_NUM; ++i) {
				close(log_events[i].fd);
				log_events[i].fd = -1;
			}
			return;
		}

		// Start logcat
		log_pid = exec_command(0, &log_fd, NULL,
							   "/system/bin/logcat",
							   "-b", "events", "-b", "main", "-b", "crash",
							   "-v", "threadtime",
							   "-s", "am_proc_start", "Magisk", "*:F",
							   NULL);
		FILE *logs = fdopen(log_fd, "r");
		while (fgets(line, sizeof(line), logs)) {
			if (line[0] == '-')
				continue;
			size_t len = strlen(line);
			pthread_mutex_lock(&lock);
			for (int i = 0; i < EVENT_NUM; ++i) {
				if (log_events[i].fd > 0 && log_events[i].filter(line))
					write(log_events[i].fd, line, len);
			}
			if (thread < 0 && log_events[HIDE_EVENT].fd < 0) {
				// New thread to handle connection to main daemon
				xpthread_create(&thread, NULL, socket_thread, NULL);
				pthread_detach(thread);
			}
			pthread_mutex_unlock(&lock);
			if (kill(log_pid, 0))
				break;
		}

		// Cleanup
		fclose(logs);
		log_fd = -1;
		kill(log_pid, SIGTERM);
		waitpid(log_pid, NULL, 0);
		test_logcat();
	}
}

/* Start new threads to monitor logcat and dump to logfile */
void monitor_logs() {
	test_logcat();
	if (loggable) {
		int fd;
		connect_daemon2(LOG_DAEMON, &fd);
		write_int(fd, DO_NOTHING);
		close(fd);
	}
}

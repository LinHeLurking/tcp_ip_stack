#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>

#include "types.h"

// #define LOG_DEBUG

enum log_level
{
	DEBUG = 0,
	INFO,
	WARNING,
	ERROR
};

static enum log_level this_log_level = DEBUG;

static const char *log_level_str[] = {"DEBUG", "INFO", "WARNING", "ERROR"};

#ifdef LOG_DEBUG
#define log_it(fmt, level_str, ...)                              \
	fprintf(stderr, "[%s:%u] %s: " fmt "\n", __FILE__, __LINE__, \
			level_str, ##__VA_ARGS__);
#else
#define log_it(fmt, level_str, ...) \
	fprintf(stderr, "%s: " fmt "\n", level_str, ##__VA_ARGS__);
#endif

#define log(level, fmt, ...)                              \
	do                                                    \
	{                                                     \
		if (level < this_log_level)                       \
			break;                                        \
		log_it(fmt, log_level_str[level], ##__VA_ARGS__); \
	} while (0)

#endif

static inline void log_cwnd_update(u32 cwnd, u32 ssthresh)
{
	FILE *fp = fopen("cwnd.txt", "a");

	struct timeval tm;
	gettimeofday(&tm, NULL);
	double stamp = tm.tv_sec * 1000 + tm.tv_usec*1.0 / 1000;
	fprintf(fp, "%lf %u %u\n", stamp, cwnd, ssthresh);
	// log(DEBUG, "CWND=%u SSTHRESH=%u", cwnd, ssthresh);

	fclose(fp);
}

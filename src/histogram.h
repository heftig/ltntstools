/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

/* The original version of this code lives in the ltntstools project,
 * so please reflect any modifications 'upstream' if this code gets
 * pulled into other projects.
 */

/* Basic histogram facility geared towards video use cases, where
 * buckets span from 1-Nms, and the finest granularity is 1ms.
 * We intensionally tradeoff a large amount of ram for fast
 * bucket update access. The default video use case takes 1MB
 * of ram to hold 16,000 buckets.
 * Generally you'd isolate all access to the histogram to a single thread.
 *
 * Use case: Measuring frame arrival times from SDI capture hardware.
 *
 *   struct ltn_histogram_s *hdl;
 *   ltn_histogram_alloc_video_defaults(&hdl, "frame arrival times");
 *
 *   // Update the histogram every time a new frame arrives.
 *   ltn_histogram_interval_update(hdl);
 *
 *   // Whenever you see fit, print the histogram content:
 *   ltn_histogram_interval_print(STDOUT_FILENO, hdl);
 *
 *   // Free the memory allocations when you're done.
 *   ltn_histogram_free(hdl);
 *
 *
 * Use case: Measuring frame encode time in a cumulative summary,
 *           for example measuring how long it took to compress a gop.
 *
 *   struct ltn_histogram_s *hdl;
 *   ltn_histogram_alloc_video_defaults(&hdl, "GOP compression time");
 *
 *   // At the start of a cumulative period, reset any counters, such as when
 *   // the GOP begins.
 *   ltn_histogram_cumulative_initialize(hdl)
 *
 *   // Measure the time it takes to compress each slice, for each and every slice in
 *   // the GOP.
 *   ltn_histogram_cumulative_begin(hdl);
 *   ..... do some processing work here, we'll measure performance.
 *   ltn_histogram_cumulative_end(hdl);
 *
 *   // Finally, when the gop is complete, flush the cumulative value into
 *   // the hisogram buckets. Don't forget to call _initialize() to reset any values
 *   // for the next GOP.
 *   ltn_histogram_cumulative_finalize(hdl);
 *   
 */

#ifndef LTN_HISTOGRAM_H
#define LTN_HISTOGRAM_H

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

struct ltn_histogram_bucket_s
{
	uint64_t count;
	struct timeval lastUpdate;
};

struct ltn_histogram_s
{
	char     name[128];
	uint64_t minValMs; /* Minimum / Maximum bucket values in ms */
	uint64_t maxValMs;
	uint64_t bucketMissCount; /* Total instances where a value was not within the valid bucket range. */
	uint32_t bucketCount;

	struct ltn_histogram_bucket_s *buckets;

	/* Interval related hisograms */
	struct timeval intervalLast;

	/* Cumulative histograms */
	uint64_t cumulativeMs;
	struct timeval cumulativeLast;
};

static __inline__ struct ltn_histogram_bucket_s *ltn_histogram_bucket(struct ltn_histogram_s *ctx, uint32_t ms)
{
	return ctx->buckets + (ms - ctx->minValMs);
}

__inline__ static int ltn_histogram_timeval_to_ms(struct timeval *tv)
{
        return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

__inline__ static int ltn_histogram_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > 1000000) {
		int nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait. tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

__inline__ void ltn_histogram_reset(struct ltn_histogram_s *ctx)
{
	memset(ctx->buckets, 0, sizeof(struct ltn_histogram_bucket_s) * ctx->bucketCount);
	gettimeofday(&ctx->intervalLast, NULL);
	ctx->bucketMissCount = 0;
	ctx->cumulativeMs = 0;
}

static __inline__ void ltn_histogram_free(struct ltn_histogram_s *ctx)
{
	if (ctx->buckets)
		free(ctx->buckets);
	free(ctx);
}

static __inline__ int ltn_histogram_alloc(struct ltn_histogram_s **handle, const char *name, uint64_t minValMs, uint64_t maxValMs)
{
	if (minValMs == maxValMs)
		return -1;
	if (maxValMs < minValMs)
		return -1;
	if (!maxValMs)
		return -1;
	if (!name)
		return -1;
	
	struct ltn_histogram_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->minValMs = minValMs;
	ctx->maxValMs = maxValMs;
	ctx->bucketCount = maxValMs - minValMs;
	strncpy(ctx->name, name, sizeof(ctx->name));
	gettimeofday(&ctx->intervalLast, NULL);

	ctx->buckets = calloc(ctx->bucketCount, sizeof(struct ltn_histogram_s));
	if (!ctx->buckets)
		return -1;

	*handle = ctx;
	return 0;
}

static __inline__ int ltn_histogram_alloc_video_defaults(struct ltn_histogram_s **handle, const char *name)
{
	return ltn_histogram_alloc(handle, name, 0, 16 * 1000);
}

static __inline__ int ltn_histogram_interval_update(struct ltn_histogram_s *ctx)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	struct timeval r;
	ltn_histogram_timeval_subtract(&r, &now, &ctx->intervalLast);
	uint32_t diffMs = ltn_histogram_timeval_to_ms(&r);

	ctx->intervalLast = now; /* Implicit struct copy. */

	if ((diffMs < ctx->minValMs) || (diffMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
		return -1;
	}

	struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, diffMs);
	bucket->lastUpdate = now; /* Implicit struct copy. */
	bucket->count++;

	return diffMs;
}

static __inline__ void ltn_histogram_interval_print(int fd, struct ltn_histogram_s *ctx)
{
	dprintf(fd, "Histogram '%s' (ms, count, last update time)\n", ctx->name);

	uint64_t cnt = 0, measurements = 0;
	for (int i = 0; i < ctx->bucketCount; i++) {
		struct ltn_histogram_bucket_s *b = &ctx->buckets[i];
		if (!b->count)
			continue;

		char timestamp[64];
		sprintf(timestamp, "%s", ctime(&b->lastUpdate.tv_sec));
		timestamp[strlen(timestamp) - 1] = 0; /* Trim trailing CR */

		dprintf(fd,
#if defined(__APPLE__)
			"-> %5" PRIu64 " %8" PRIu64 "  %s (%ld.%d)\n",
#endif
#if defined(__linux__)
			"-> %5" PRIu64 " %8" PRIu64 "  %s (%d.%d)\n",
#endif
			ctx->minValMs + i,
			b->count,
			timestamp,
			b->lastUpdate.tv_sec,
			b->lastUpdate.tv_usec);

		cnt++;
		measurements += b->count;
	}

	if (ctx->bucketMissCount) {
		dprintf(fd, "%" PRIu64 " out-of-range bucket misses\n", ctx->bucketMissCount);
	}

	dprintf(fd, "%" PRIu64 " distinct buckets with %" PRIu64 " total measurements, range: %" PRIu64 " -> %" PRIu64 " ms\n",
		cnt,
		measurements,
		ctx->minValMs, ctx->maxValMs);
}

__inline__ static void ltn_histogram_cumulative_initialize(struct ltn_histogram_s *ctx)
{
	ctx->cumulativeMs = 0;
}

__inline__ static void ltn_histogram_cumulative_begin(struct ltn_histogram_s *ctx)
{
	gettimeofday(&ctx->cumulativeLast, 0);
}

__inline__ static uint64_t ltn_histogram_cumulative_end(struct ltn_histogram_s *ctx)
{
	struct timeval now;
	gettimeofday(&now, 0);

	struct timeval r;
	ltn_histogram_timeval_subtract(&r, &now, &ctx->cumulativeLast);

	uint64_t val = ltn_histogram_timeval_to_ms(&r);
	ctx->cumulativeMs += val;

	return val;
}

__inline__ static uint64_t ltn_histogram_cumulative_finalize(struct ltn_histogram_s *ctx)
{
	/* Write ctx->cumulativeMs into the buckets. */
	if ((ctx->cumulativeMs < ctx->minValMs) || (ctx->cumulativeMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
	} else {
		struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, ctx->cumulativeMs);
		gettimeofday(&bucket->lastUpdate, 0);
		bucket->count++;
	}

	return ctx->cumulativeMs;
}

#endif /* LTN_HISTOGRAM_H */


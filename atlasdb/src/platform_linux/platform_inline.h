/* **********************************************************
 * Copyright 2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

#ifndef PLATFORM_LINUX_INLINE_H
#define PLATFORM_LINUX_INLINE_H

#include <laio.h>
#include <string.h> // for memcpy, strerror
#include <time.h> // for nanosecond sleep api.

static inline size_t
platform_strnlen(const char *s, size_t maxlen)
{
   return strnlen(s, maxlen);
}

static inline uint32
platform_popcount(uint32 x) { return __builtin_popcount(x); }

/*
 * Because platform_checksum32 is used as a function pointer, we want a
 * macro instead of an inline function. The latter would result in an
 * extra function call indirection.
 */
#define platform_checksum32 XXH32

static inline checksum64
platform_checksum64(const void *data,
                    size_t      len,
                    uint64      seed)
{
   return XXH64(data, len, seed);
}

static inline checksum128
platform_checksum128(const void   *input,
                     size_t        length,
                     uint64        seed)
{
   return XXH128(input, length, seed);
}

static inline bool
platform_checksum_is_equal(checksum128 left,
                           checksum128 right)
{
   return XXH128_isEqual(left, right);
}

static inline void
platform_free_from_heap(platform_heap_id UNUSED_PARAM(heap_id),
                        void *ptr)
{
   free(ptr);
}

static inline timestamp
platform_get_timestamp(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return SEC_TO_NSEC(ts.tv_sec) + ts.tv_nsec;
}

static inline timestamp
platform_timestamp_elapsed(timestamp tv)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return SEC_TO_NSEC(ts.tv_sec) + ts.tv_nsec - tv;
}

static inline timestamp
platform_timestamp_diff(timestamp start,
                        timestamp end)
{
   return end - start;
}

static inline timestamp
platform_get_real_time(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return SEC_TO_NSEC(ts.tv_sec) + ts.tv_nsec;
}

static inline void
platform_pause()
{
   __builtin_ia32_pause();
}

static inline void
platform_sleep(uint64 ns)
{
   if (ns < USEC_TO_NSEC(50)) {
      for (uint64 i = 0; i < ns / 5 + 1; i++) {
         platform_pause();
      }
   } else {
      struct timespec res;
      res.tv_sec = ns / SEC_TO_NSEC(1);
      res.tv_nsec = (ns - (res.tv_sec * SEC_TO_NSEC(1)));
      clock_nanosleep(CLOCK_MONOTONIC, 0, &res, NULL);
   }

}

static inline void
platform_semaphore_destroy(platform_semaphore *sema)
{
   __attribute__ ((unused)) int err = sem_destroy(sema);
   debug_assert(!err);
}

static inline void
platform_semaphore_init(platform_semaphore *sema,
                        int value,
                        platform_heap_id UNUSED_PARAM(heap_id))
{
   __attribute__ ((unused)) int err = sem_init(sema, 0, value);
   debug_assert(!err);
}

static inline void
platform_semaphore_post(platform_semaphore *sema)
{
   __attribute__ ((unused)) int err = sem_post(sema);
   debug_assert(!err);
}

static inline void
platform_semaphore_wait(platform_semaphore *sema)
{
   __attribute__ ((unused)) int err = sem_wait(sema);
   debug_assert(!err);
}

/*
 * STATUS_OK: if wait succeeded
 * STATUS_BUSY: if need to retry
 * other: failure
 */
static inline platform_status
platform_semaphore_try_wait(platform_semaphore *sema)
{
   int ret = sem_trywait(sema);

   if (ret == 0) {
      return STATUS_OK;
   }
   if (errno == EAGAIN) {
      return STATUS_BUSY;
   }

   return CONST_STATUS(errno);
}

static inline platform_status
platform_mutex_lock(platform_mutex *mu)
{
   int ret;

   ret = pthread_mutex_lock(mu);

   return CONST_STATUS(ret);
}

static inline platform_status
platform_mutex_unlock(platform_mutex *mu)
{
   int ret;

   ret = pthread_mutex_unlock(mu);

   return CONST_STATUS(ret);
}

static inline threadid
platform_get_tid()
{
   extern __thread threadid xxxtid;
   return xxxtid;
}

static inline void
platform_set_tid(threadid t)
{
   extern __thread threadid xxxtid;
   xxxtid = t;
}

static inline void platform_yield() {}

// platform predicates
static inline bool
STATUS_IS_EQ(const platform_status s1,
             const platform_status s2)
{
   return s1.r == s2.r;
}

static inline bool
STATUS_IS_NE(const platform_status s1,
             const platform_status s2)
{
   return s1.r != s2.r;
}

static inline const char *
platform_status_to_string(const platform_status status)
{
   return strerror(status.r);
}

#define PLATFORM_DEFAULT_LOG_HANDLE stdout
#define PLATFORM_ERR_LOG_HANDLE stderr
#define PLATFORM_CR "\r"

#define platform_open_log_stream()              \
   char *bp;                                    \
   size_t size;                                 \
   platform_stream_handle stream;               \
   stream = open_memstream(&bp, &size);

#define platform_close_log_stream(h)            \
   fclose(stream);                              \
   fputs(bp, h);                                \
   fflush(h);                                   \
   platform_free(NULL, bp);

/*
 * platform_log_stream should be called between platform_open_log_stream and
 * platform_close_log_stream.
 */
#define platform_log_stream(...)                \
   do {                                         \
      fprintf(stream, __VA_ARGS__);             \
   } while (0)

#define platform_log(...)                       \
   do {                                         \
      printf(__VA_ARGS__);                      \
   } while (0)

#define platform_throttled_log(sec, ...)        \
   do {                                         \
      platform_log(__VA_ARGS__);                \
   } while (0)

#define platform_default_log(...)               \
   do {                                         \
      fprintf(stdout, __VA_ARGS__);             \
      fflush(stdout);                           \
   } while (0)

#define platform_error_log(...)                 \
   do {                                         \
      fprintf(stderr, __VA_ARGS__);             \
      fflush(stderr);                           \
   } while (0)

#define platform_throttled_error_log(sec, ...)  \
   do {                                         \
      platform_error_log(__VA_ARGS__);          \
   } while (0)

#define platform_handle_log(lh, ...)            \
   do {                                         \
      fprintf(lh, __VA_ARGS__);                 \
      fflush(lh);                               \
   } while (0)

#define platform_open_log_file(path, mode) ({   \
   platform_log_handle lh = fopen(path, mode);  \
   platform_assert(lh);                         \
   lh;                                          \
})

#define platform_close_log_file(path)           \
   do {                                         \
      fclose(path);                             \
   } while (0)

#define platform_thread_cleanup_push(func, arg) \
   pthread_cleanup_push((func), (arg))

#define platform_thread_cleanup_pop(exec) \
   pthread_cleanup_pop((exec))

#define splinter_list_insert(...)
#define splinter_list_remove(...)

static inline void
platform_histo_insert(platform_histo_handle histo, int64 datum)
{
   int lo = 0, hi = histo->num_buckets - 1;

   while (hi > lo) {
      int mid = lo + (hi - lo) / 2;

      if (datum > histo->bucket_limits[mid]) {
         lo = mid + 1;
      } else {
         hi = mid - 1;
      }
   }
   platform_assert(lo < histo->num_buckets);
   histo->count[lo]++;
   if (histo->num == 0) {
      histo->min = histo->max = datum;
   } else {
      histo->max = MAX(histo->max, datum);
      histo->min = MIN(histo->min, datum);
   }
   histo->total += datum;
   histo->num++;
}

static inline void
platform_histo_merge_in(platform_histo_handle dest_histo,
                        platform_histo_handle src_histo)
{
   uint32 i;
   if (src_histo->num == 0) {
      return;
   }

   platform_assert(dest_histo->num_buckets == src_histo->num_buckets);
   for (i = 0; i < dest_histo->num_buckets - 1; i++) {
      platform_assert(dest_histo->bucket_limits[i] == src_histo->bucket_limits[i]);
   }
   if (src_histo->min < dest_histo->min || dest_histo->num == 0) {
      dest_histo->min = src_histo->min;
   }
   if (src_histo->max > dest_histo->max || dest_histo->num == 0) {
      dest_histo->max = src_histo->max;
   }
   dest_histo->total += src_histo->total;
   dest_histo->num += src_histo->num;

   for (i = 0; i < dest_histo->num_buckets; i++) {
      dest_histo->count[i] += src_histo->count[i];
   }
}

static inline platform_heap_id
platform_get_heap_id(void)
{
   // void* NULL since we don't actually need a heap id
   return NULL;
}

static inline platform_module_id
platform_get_module_id()
{
   // void* NULL since we don't actually need a module id
   return NULL;
}

static inline void *
platform_aligned_malloc(const platform_heap_id UNUSED_PARAM(heap_id),
                        const size_t alignment, // IN
                        const size_t size)      // IN
{
   // Requirement for aligned_alloc
   platform_assert(IS_POWER_OF_2(alignment));

   /*
    * aligned_alloc requires size to be a multiple of alignment
    * round up to nearest multiple of alignment
    *
    * Note that since this is inlined, the compiler will turn the constant
    * (power of 2) alignment mod operations into bitwise &
    */
   const size_t padding = (alignment - (size % alignment)) % alignment;
   return aligned_alloc(alignment, size + padding);
}

static inline platform_status
platform_condvar_lock(platform_condvar *cv)
{
   int status;

   status = pthread_mutex_lock(&cv->lock);
   return CONST_STATUS(status);
}

static inline platform_status
platform_condvar_unlock(platform_condvar *cv)
{
   int status;

   status = pthread_mutex_unlock(&cv->lock);
   return CONST_STATUS(status);
}

static inline void
platform_condvar_destroy(platform_condvar *cv)
{
   pthread_mutex_destroy(&cv->lock);
   pthread_cond_destroy(&cv->cond);
}

#endif
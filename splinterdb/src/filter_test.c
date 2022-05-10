/* ************************************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * ************************************************************************/

/*
 * filter_test.c --
 *
 *     This file contains the test interfaces for the filter
 */
#include "platform.h"

#include "data.h"
#include "test.h"
#include "quick_filter.h"
#include "allocator.h"
#include "rc_allocator.h"
#include "shard_log.h"
#include "cache.h"
#include "clockcache.h"
#include "memcache.h"
#include "util.h"
#define TEST_MAX_ASYNC_INFLIGHT 0
static uint64 max_async_inflight = 0;

#include "poison.h"

static platform_status
test_filter_basic(cache               *cc,
                  quick_filter_config *cfg,
                  platform_heap_id     hid,
                  uint64               num_fingerprints,
                  uint64               fanout)
{
   platform_log("filter_test: filter basic test started\n");
   uint64 filter_addr;
   uint32 *fingerprint_arr = TYPED_ARRAY_MALLOC(hid,
                                                fingerprint_arr,
                                                num_fingerprints);
   uint64 i;
   const uint64 key_size = cfg->data_cfg->key_size;
   char key[MAX_KEY_SIZE];
   uint64 false_positives = 0;
   platform_status rc;

   if (key_size < sizeof(uint64)) {
      platform_log("key_size %lu too small\n", key_size);
      return STATUS_BAD_PARAM;
   }
   for (i = 0; i < num_fingerprints; i++) {
      memset(key, 0, key_size);
      *(uint64 *)key = i;
      fingerprint_arr[i] = cfg->hash(key, key_size, cfg->seed);
   }

   rc = quick_filter_init(cc, cfg, hid, num_fingerprints, fingerprint_arr,
                          &filter_addr);
   if (!SUCCESS(rc)) {
      platform_error_log("quick_filter_init failed: %s\n",
                         platform_status_to_string(rc));
      goto destroy_fingerprint_arr;
   }

   //quick_filter_print(cc, cfg, num_fingerprints, filter_addr);

   for (i = 0; i < num_fingerprints; i++) {
      memset(key, 0, key_size);
      *(uint64 *)key = i;
      int found = quick_filter_lookup(cc, cfg, filter_addr, num_fingerprints,
                                      key);
      if (found != 1) {
         platform_log("key %lu not found in filter\n", i);
         return STATUS_NOT_FOUND;
      }
   }

   for (i = num_fingerprints; i < 2 * num_fingerprints; i++) {
      memset(key, 0, key_size);
      *(uint64 *)key = i;
      int found = quick_filter_lookup(cc, cfg, filter_addr, num_fingerprints,
                                      key);
      if (found == 1)
         false_positives++;
   }
   rc = STATUS_OK;

   fraction false_positive_rate = init_fraction(false_positives,
                                                num_fingerprints);
   platform_log("filter_basic_test: false positive rate "FRACTION_FMT(1, 2),
                FRACTION_ARGS(false_positive_rate));
   quick_filter_deinit(cc, cfg, filter_addr);

destroy_fingerprint_arr:
   platform_free(hid, fingerprint_arr);
   return rc;
}

// A single async context
typedef struct {
   quick_async_ctxt ctxt;
   cache_async_ctxt cache_ctxt;
   bool             ready;
   char             key[MAX_KEY_SIZE];
} filter_test_async_ctxt;

// Per-table array of async contexts
typedef struct {
   // 1: async index is available, 0: async index is used
   uint64                  ctxt_bitmap;
   filter_test_async_ctxt  ctxt[TEST_MAX_ASYNC_INFLIGHT];
} filter_test_async_lookup;

static void
filter_test_async_callback(quick_async_ctxt *quick_ctxt)
{
   filter_test_async_ctxt *ctxt =
      container_of(quick_ctxt, filter_test_async_ctxt, ctxt);

   platform_assert(!ctxt->ready);
   /*
    * A real application callback will need to requeue the async
    * lookup back to the per-thread async task queue. But our test
    * just retries anything that's ready, so this callback is simple.
    */
   ctxt->ready = TRUE;
}

static filter_test_async_ctxt *
filter_test_get_async_ctxt(filter_test_async_lookup *async_lookup)
{
   filter_test_async_ctxt *ctxt;
   int idx;
   uint64 old = async_lookup->ctxt_bitmap;

   idx = __builtin_ffsl(old);
   if (idx == 0) {
      return NULL;
   }
   idx = idx - 1;
   async_lookup->ctxt_bitmap = old & ~(1UL<<idx);
   ctxt = &async_lookup->ctxt[idx];
   quick_filter_ctxt_init(&ctxt->ctxt, &ctxt->cache_ctxt,
                          filter_test_async_callback);
   ctxt->ready = FALSE;

   return ctxt;
}

static void
filter_test_put_async_ctxt(filter_test_async_lookup *async_lookup,
                           filter_test_async_ctxt   *ctxt)
{
   int idx = ctxt - async_lookup->ctxt;

   debug_assert(idx >= 0 && idx < max_async_inflight);
   async_lookup->ctxt_bitmap |= (1UL << idx);
}

static void
filter_test_async_ctxt_init(filter_test_async_lookup *async_lookup)
{
   _Static_assert(8 * sizeof(async_lookup->ctxt_bitmap) >
                  TEST_MAX_ASYNC_INFLIGHT, "Not enough bits for bitmap");
   async_lookup->ctxt_bitmap = (1UL<<max_async_inflight)-1;
}

static bool
filter_test_async_ctxt_is_used(const filter_test_async_lookup *async_lookup,
                               int                             ctxt_idx)
{
   debug_assert(ctxt_idx >= 0 && ctxt_idx < max_async_inflight);
   return async_lookup->ctxt_bitmap & (1UL << ctxt_idx) ? FALSE : TRUE;
}

static bool
filter_test_async_ctxt_any_used(const filter_test_async_lookup  *async_lookup)
{
   debug_assert((async_lookup->ctxt_bitmap &
                 ~((1UL << max_async_inflight) - 1)) == 0);
   return async_lookup->ctxt_bitmap != (1UL << max_async_inflight) - 1;
}

static bool
filter_test_run_pending(cache                    *cc,
                        quick_filter_config      *cfg,
                        uint64                    filter_addr,
                        uint64                    num_fingerprints,
                        filter_test_async_lookup *async_lookup,
                        filter_test_async_ctxt   *skip_ctxt,
                        bool                      positive_lookup,
                        uint64                   *false_positives)
{
   int i;

   debug_assert(positive_lookup == (false_positives == NULL));
   if (!filter_test_async_ctxt_any_used(async_lookup)) {
      return FALSE;
   }
   for (i = 0; i < max_async_inflight; i++) {
      if (!filter_test_async_ctxt_is_used(async_lookup, i)) {
         continue;
      }
      int found;
      cache_async_result res;
      filter_test_async_ctxt *ctxt = &async_lookup->ctxt[i];
      // We skip skip_ctxt, because that it just asked us to retry.
      if (ctxt == skip_ctxt || !ctxt->ready) {
         continue;
      }
      ctxt->ready = FALSE;
      res = quick_filter_lookup_async(cc, cfg, filter_addr, num_fingerprints,
                                      ctxt->key, &found, &ctxt->ctxt);
      switch (res) {
      case async_locked:
      case async_no_reqs:
         ctxt->ready = TRUE;
         break;
      case async_io_started:
         break;
      case async_success:
         if (positive_lookup) {
            if (!found) {
               platform_log("key %lu not found in filter %lu\n",
                            *(uint64 *)ctxt->key, filter_addr);
               platform_assert(0);
            }
         } else {
            if (found) {
               (*false_positives)++;
            }
         }
         filter_test_put_async_ctxt(async_lookup, ctxt);
         break;
      default:
         platform_assert(0);
      }
   }

   return TRUE;
}

static platform_status
test_filter_perf(cache               *cc,
                 quick_filter_config *cfg,
                 platform_heap_id     hid,
                 uint64               num_fingerprints,
                 uint64               fanout,
                 uint64               num_trees)
{
   platform_log("filter_test: filter perf test started\n");
   uint64 *filter_addr = TYPED_ARRAY_MALLOC(hid, filter_addr, num_trees);
   platform_assert(filter_addr);
   uint32 *fingerprint_arr = TYPED_ARRAY_MALLOC(hid,
                                                fingerprint_arr,
                                                num_fingerprints * num_trees);
   filter_test_async_lookup *async_lookup =
      TYPED_ARRAY_MALLOC(hid, async_lookup, num_trees);
   platform_assert(fingerprint_arr);
   uint64 i, j, num_async = 0;
   const uint64 key_size = cfg->data_cfg->key_size;
   char key[MAX_KEY_SIZE];
   uint64 false_positives = 0;
   platform_status rc = STATUS_OK;

   if (key_size < sizeof(uint64)) {
      platform_log("key_size %lu too small\n", key_size);
      return STATUS_BAD_PARAM;
   }
   for (i = 0; i < num_fingerprints * num_trees; i++) {
      memset(key, 0, key_size);
      *(uint64 *)key = i;
      fingerprint_arr[i] = cfg->hash(key, key_size, cfg->seed);
   }

   uint64 start_time = platform_get_timestamp();
   for (i = 0; i < num_trees; i++) {
      rc = quick_filter_init(cc, cfg, hid, num_fingerprints,
                             fingerprint_arr + (num_fingerprints * i),
                             &filter_addr[i]);
      if (!SUCCESS(rc)) {
         platform_error_log("quick_filter_init failed: %s\n",
                            platform_status_to_string(rc));
         goto destroy_arrays;
      }
      filter_test_async_ctxt_init(&async_lookup[i]);
   }
   platform_log("filter insert time per key %lu ns\n",
         platform_timestamp_elapsed(start_time) / (num_fingerprints * num_trees));

   uint64 total_pages = 0;
   for (i = 0; i < num_trees; i++) {
     total_pages += quick_filter_num_pages(cc, cfg, num_fingerprints, filter_addr[i]);
   }
   platform_log("filter bits/key %f\n",
                8.0 * total_pages * cache_page_size(cc) / (num_trees * num_fingerprints));

   //quick_filter_print(cc, cfg, num_fingerprints, filter_addr);

   start_time = platform_get_timestamp();
   for (i = 0; i < num_fingerprints; i++) {
      for (j = 0; j < num_trees; j++) {
         filter_test_async_ctxt *ctxt;
         int found;

         ctxt = filter_test_get_async_ctxt(&async_lookup[j]);
         if (ctxt == NULL) {
            memset(key, 0, key_size);
            *(uint64 *)key = j * num_fingerprints + i;
            found = quick_filter_lookup(cc, cfg, filter_addr[j],
                                        num_fingerprints, key);
            if (found != 1) {
               platform_log("key %lu not found in filter %lu\n",
                            j * num_fingerprints + i, j);
               return STATUS_NOT_FOUND;
            }
         } else {
            cache_async_result res;

            num_async++;
            quick_filter_ctxt_init(&ctxt->ctxt, &ctxt->cache_ctxt,
                                   filter_test_async_callback);
            memset(ctxt->key, 0, key_size);
            *(uint64 *)ctxt->key = j * num_fingerprints + i;
            res = quick_filter_lookup_async(cc, cfg, filter_addr[j],
                                            num_fingerprints, ctxt->key,
                                            &found, &ctxt->ctxt);
            switch (res) {
            case async_locked:
            case async_no_reqs:
               ctxt->ready = TRUE;
               break;
            case async_io_started:
               ctxt = NULL;
               break;
            case async_success:
               if (!found) {
                  platform_log("key %lu not found in filter %lu\n",
                               j * num_fingerprints + i, j);
                  return STATUS_NOT_FOUND;
               }
               filter_test_put_async_ctxt(&async_lookup[j], ctxt);
               ctxt = NULL;
               break;
            default:
               platform_assert(0);
            }
         }
         filter_test_run_pending(cc, cfg, filter_addr[j], num_fingerprints,
                                 &async_lookup[j], ctxt, TRUE, NULL);
      }
   }
   for (j = 0; j < num_trees; j++) {
      // Rough detection of stuck contexts
      const timestamp ts = platform_get_timestamp();
      while (filter_test_run_pending(cc, cfg, filter_addr[j], num_fingerprints,
                                     &async_lookup[j], NULL, TRUE, NULL)) {
         cache_cleanup(cc);
         platform_assert(platform_timestamp_elapsed(ts) <
                         TEST_STUCK_IO_TIMEOUT);
      }
   }
   platform_log("filter positive lookup time per key %lu ns\n",
         platform_timestamp_elapsed(start_time) / (num_fingerprints * num_trees));
   platform_log("%lu%% lookups were async\n",
                num_async * 100 / (num_fingerprints * num_trees));

   start_time = platform_get_timestamp();
   num_async = 0;
   for (i = num_fingerprints; i < 2 * num_fingerprints; i++) {
      for (j = 0; j < num_trees; j++) {
         filter_test_async_ctxt *ctxt;
         int found;

         ctxt = filter_test_get_async_ctxt(&async_lookup[j]);
         if (ctxt == NULL) {
            memset(key, 0, key_size);
            *(uint64 *)key = j * num_fingerprints + i;
            found = quick_filter_lookup(cc, cfg, filter_addr[j],
                                            num_fingerprints, key);
            if (found == 1) {
               false_positives++;
            }
         } else {
            cache_async_result res;

            num_async++;
            quick_filter_ctxt_init(&ctxt->ctxt, &ctxt->cache_ctxt,
                                   filter_test_async_callback);
            memset(ctxt->key, 0, key_size);
            *(uint64 *)ctxt->key = j * num_fingerprints + i;
            res = quick_filter_lookup_async(cc, cfg, filter_addr[j],
                                            num_fingerprints, ctxt->key,
                                            &found, &ctxt->ctxt);
            switch (res) {
            case async_locked:
            case async_no_reqs:
               ctxt->ready = TRUE;
               break;
            case async_io_started:
               ctxt = NULL;
               break;
            case async_success:
               if (found == 1) {
                  false_positives++;
               }
               filter_test_put_async_ctxt(&async_lookup[j], ctxt);
               ctxt = NULL;
               break;
            default:
               platform_assert(0);
            }
         }
         filter_test_run_pending(cc, cfg, filter_addr[j], num_fingerprints,
                                 &async_lookup[j], ctxt, FALSE,
                                 &false_positives);
      }
   }
   for (j = 0; j < num_trees; j++) {
      // Rough detection of stuck contexts
      const timestamp ts = platform_get_timestamp();
      while (filter_test_run_pending(cc, cfg, filter_addr[j], num_fingerprints,
                                     &async_lookup[j], NULL, FALSE,
                                     &false_positives)) {
         cache_cleanup(cc);
         platform_assert(platform_timestamp_elapsed(ts) <
                         TEST_STUCK_IO_TIMEOUT);
      }
   }
   platform_log("filter negative lookup time per key %lu ns\n",
         platform_timestamp_elapsed(start_time) / (num_fingerprints * num_trees));
   fraction false_positive_rate = init_fraction(false_positives,
                                                num_fingerprints * num_trees);
   platform_log("filter_basic_test: false positive rate " FRACTION_FMT(1, 6)" for "
                "%lu trees\n", FRACTION_ARGS(false_positive_rate),
                num_trees);
   platform_log("%lu%% lookups were async\n",
                num_async * 100 / (num_fingerprints * num_trees));

   for (i = 0; i < num_trees; i++)
      quick_filter_deinit(cc, cfg, filter_addr[i]);
   cache_print_stats(cc);

destroy_arrays:
   platform_free(hid, async_lookup);
   platform_free(hid, fingerprint_arr);
   platform_free(hid, filter_addr);
   return rc;
}

static void
usage(const char *argv0) {
   platform_error_log("Usage:\n"
                      "\t%s\n"
                      "\t%s --perf --max-async-inflight [num]\n",
                      argv0,
                      argv0);
   config_usage();
}

int
filter_test(int argc, char *argv[])
{
   int                   r;
   data_config           data_cfg;
   io_config             io_cfg;
   rc_allocator_config   allocator_cfg;
   clockcache_config     cache_cfg;
   shard_log_config      log_cfg;
   rc_allocator          al;
   clockcache           *cc;
   memcache              mc;
   cache                *cp;
   int                   config_argc;
   char                **config_argv;
   bool                  run_perf_test = FALSE;
   platform_status       rc;
   uint64                seed;
   task_system          *ts;
   bool                  found_filter_test_arg = TRUE;
   bool                  use_memcache = FALSE;

   config_argc = argc - 1;
   config_argv = argv + 1;
   while (0 < config_argc && found_filter_test_arg) {
     found_filter_test_arg = FALSE;
     if (strcmp(config_argv[0], "--perf") == 0) {
       run_perf_test = TRUE;
       config_argc--;
       config_argv++;
       found_filter_test_arg = TRUE;
     } else if (strcmp(config_argv[0], "--use-memcache") == 0) {
       use_memcache = TRUE;
       config_argc--;
       config_argv++;
       found_filter_test_arg = TRUE;
     }
   }
   if (config_argc > 0 && strncmp(config_argv[0], "--max-async-inflight",
                                  sizeof("--max-async-inflight")) == 0) {
      if (!try_string_to_uint64(config_argv[1], &max_async_inflight)) {
         usage(argv[0]);
         return -1;
      }
      config_argc -= 2;
      config_argv += 2;
   }

   // Create a heap for io, allocator, cache and splinter
   platform_heap_handle hh;
   platform_heap_id hid;
   rc = platform_heap_create(platform_get_module_id(), 1 * GiB, &hh, &hid);
   platform_assert_status_ok(rc);

   splinter_config *cfg = TYPED_MALLOC(hid, cfg);

   rc = test_parse_args(cfg, &data_cfg, &io_cfg, &allocator_cfg, &cache_cfg,
                        &log_cfg, &seed, config_argc, config_argv);
   if (!SUCCESS(rc)) {
      platform_error_log("filter_test: failed to parse config: %s\n",
                         platform_status_to_string(rc));
      /*
       * Provided arguments but set things up incorrectly.
       * Print usage so client can fix commandline.
       */
      usage(argv[0]);
      r = -1;
      goto cleanup;
   }

   platform_io_handle *io = TYPED_MALLOC(hid, io);
   platform_assert(io != NULL);
   rc = io_handle_init(io, &io_cfg, hh, hid);
   if (!SUCCESS(rc)) {
      goto free_iohandle;
   }

   uint8 num_bg_threads[NUM_TASK_TYPES] = { 0 }; // no bg threads
   rc = test_init_splinter(hid, io, &ts, cfg->use_stats, FALSE, num_bg_threads);
   if (!SUCCESS(rc)) {
      platform_error_log("Failed to init splinter ts: %s\n",
                         platform_status_to_string(rc));
      goto deinit_iohandle;
   }

   rc = rc_allocator_init(&al, &allocator_cfg, (io_handle *)io, hh, hid,
                          platform_get_module_id());
   platform_assert_status_ok(rc);

   if (use_memcache) {
     rc = memcache_init(&mc, cache_cfg.capacity);
     platform_assert_status_ok(rc);
     cp = (cache *)&mc;
   } else {
     cc = TYPED_MALLOC(hid, cc);
     platform_assert(cc);
     rc = clockcache_init(cc, &cache_cfg, (io_handle *)io, (allocator *)&al,
                          "test", ts, hh, hid, platform_get_module_id());
     platform_assert_status_ok(rc);
     cp = (cache *)cc;
   }

   if (run_perf_test) {
     rc = test_filter_perf(cp, &cfg->filter_cfg, hid,
            cfg->mt_cfg.max_tuples_per_memtable, cfg->fanout, 100);
      platform_assert(SUCCESS(rc));
      rc = test_filter_perf(cp, &cfg->filter_cfg, hid,
            cfg->mt_cfg.max_tuples_per_memtable / 5, cfg->fanout, 100);
      platform_assert(SUCCESS(rc));
   } else {
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            cfg->mt_cfg.max_tuples_per_memtable, cfg->fanout);
      platform_assert(SUCCESS(rc));
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            cfg->mt_cfg.max_tuples_per_memtable / 5, cfg->fanout);
      platform_assert(SUCCESS(rc));
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            100, cfg->fanout);
      platform_assert(SUCCESS(rc));
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            50, cfg->max_branches_per_node);
      platform_assert(SUCCESS(rc));
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            1, cfg->fanout);
      platform_assert(SUCCESS(rc));
      rc = test_filter_basic(cp, &cfg->filter_cfg, hid,
            1, 2 * cfg->fanout);
      platform_assert(SUCCESS(rc));
   }

   if (use_memcache) {
     memcache_deinit(&mc);
   } else {
     clockcache_deinit(cc);
     platform_free(hid, cc);
   }
   rc_allocator_deinit(&al);
   test_deinit_splinter(hid, ts);
deinit_iohandle:
   io_handle_deinit(io);
free_iohandle:
   platform_free(hid, io);
   r = 0;
cleanup:
   platform_free(hid, cfg);
   platform_heap_destroy(&hh);

   return r;
}

/* **********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * quick_filter.h --
 *
 *     This file contains the quick_filter interface.
 */

#ifndef __QUICK_FILTER_H
#define __QUICK_FILTER_H

#include "cache.h"
#include "iterator.h"
#include "data.h"
#include "platform.h"
#include "util.h"

// uint32 aligned to conserve space on disk
typedef struct quick_filter {
   uint64 root_addr;
   uint32 opaque_data;
} PACKED UINT32_ALIGNED quick_filter;
_Static_assert(sizeof(quick_filter) == sizeof(uint64) + sizeof(uint32),
               "quick_filter has padding");

typedef struct quick_filter_config {
   uint64 remainder_size;
   uint64 index_size;

   hash_fn hash;
   unsigned int seed;

   // These must match the cache/fs/etc.
   uint64 page_size;
   uint64 addrs_per_page;
   uint64 extent_size;
   /*
    * We intentionally do not provide the data config for range delete message
    * tree.  Filters would not work for range deletes.
    */
   // data config of point message tree
   data_config *data_cfg;
} quick_filter_config;

// These are generally calculated from the config and num_fingerprints
typedef struct quick_filter_params {
   uint32 remainder_mask;
   uint64 num_buckets;
   uint64 bucket_size;
   uint64 num_fingerprints;
} quick_filter_params;

struct quick_async_ctxt;
typedef void (*quick_async_cb)(struct quick_async_ctxt *ctxt);

// States for the filter async lookup.
typedef enum {
   quick_async_state_start,
   quick_async_state_get_index,     // re-entrant state
   quick_async_state_get_filter,    // re-entrant state
   quick_async_state_got_index,
   quick_async_state_got_filter,
} quick_async_state;

// Context of a filter async lookup request
typedef struct quick_async_ctxt {
   /*
    * When async lookup returns async_io_started, it uses this callback to
    * inform the upper layer that the page needed by async filter lookup
    * has been loaded into the cache, and the upper layer should re-enqueue
    * the async filter lookup for dispatch.
    */
   quick_async_cb          cb;
   // Internal fields
   quick_async_state       prev_state;   // Previous state
   quick_async_state       state;        // Current state
   quick_filter_params     params;       // Filter params
   bool                    was_async;    // Was the last cache_get async ?
   uint32                  remainder;    // remainder
   uint64                  bucket;       // hash bucket
   uint64                  index;        // hash index
   uint64                  page_addr;    // Can be index or filter
   uint64                  header_addr;  // header address in filter page
   cache_async_ctxt       *cache_ctxt;   // cache ctxt for async get
} quick_async_ctxt;

int
quick_filter_lookup(cache               *cc,
                    quick_filter_config *cfg,
                    uint64               filter_addr,
                    uint64               num_fingerprints,
                    char                *key);

/*
 *-----------------------------------------------------------------------------
 *
 * quick_filter_ctxt_init --
 *
 *      Initialized the async context used by an async filter request.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static inline void
quick_filter_ctxt_init(quick_async_ctxt *ctxt,       // OUT
                       cache_async_ctxt *cache_ctxt, // IN
                       quick_async_cb    cb)         // IN
{
   ctxt->state      = quick_async_state_start;
   ctxt->cb         = cb;
   ctxt->cache_ctxt = cache_ctxt;
}

cache_async_result
quick_filter_lookup_async(cache               *cc,
                          quick_filter_config *cfg,
                          uint64               filter_addr,
                          uint64               num_fingerprints,
                          char                *key,
                          int                 *found,
                          quick_async_ctxt    *ctxt);

platform_status
quick_filter_init(cache               *cc,
                  quick_filter_config *cfg,
                  platform_heap_id     hid,
                  uint64               num_fingerprints,
                  uint32              *fingerprint_array,
                  uint64              *filter_addr);

void
quick_filter_deinit(cache               *cc,
                    quick_filter_config *cfg,
                    uint64               filter_addr);

void
quick_filter_verify(cache               *cc,
                    quick_filter_config *cfg,
                    uint64               filter_addr,
                    uint64               num_tuples,
                    iterator            *itor);

void
quick_filter_print(cache               *cc,
                   quick_filter_config *cfg,
                   uint64               num_fingerprints,
                   uint64               filter_addr);


uint64 quick_filter_num_pages(cache               *cc,
                              quick_filter_config *cfg,
                              uint64               num_fingerprints,
                              uint64               filter_addr);
#endif // __QUICK_FILTER_H

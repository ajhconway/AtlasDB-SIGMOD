/*
 * routing_filter.h --
 *
 *     This file contains the routing_filter interface.
 */

#ifndef __ROUTING_FILTER_H
#define __ROUTING_FILTER_H

#include "cache.h"
#include "iterator.h"
#include "data.h"
#include "util.h"
#include "platform.h"

#define MAX_FILTERS 32
#define ROUTING_NOT_FOUND (UINT16_MAX)

// In splinter, there is an strict max number of compacted tuples in a node, so
// we used routing filters designed for that many keys. This allows us not to
// worry about how many bits to store etc.

typedef struct routing_config {
   uint32 fingerprint_size;
   uint32 index_size;
   uint32 log_index_size;

   hash_fn hash;
   unsigned int seed;

   // These must match the cache/fs/etc.
   uint64 page_size;
   uint64 extent_size;
   data_config *data_cfg;
} routing_config;

typedef struct PACKED routing_filter {
   uint64 addr;
   uint64 meta_head;
   uint32 num_fingerprints;
   uint32 num_unique;
   uint32 value_size;
} routing_filter;

struct routing_async_ctxt;
typedef void (*routing_async_cb)(struct routing_async_ctxt *ctxt);

// States for the filter async lookup.
typedef enum {
   routing_async_state_start,
   routing_async_state_get_index,     // re-entrant state
   routing_async_state_get_filter,    // re-entrant state
   routing_async_state_got_index,
   routing_async_state_got_filter,
} routing_async_state;

// Context of a filter async lookup request
typedef struct routing_async_ctxt {
   /*
    * When async lookup returns async_io_started, it uses this callback to
    * inform the upper layer that the page needed by async filter lookup
    * has been loaded into the cache, and the upper layer should re-enqueue
    * the async filter lookup for dispatch.
    */
   routing_async_cb     cb;
   // Internal fields
   routing_async_state  prev_state;   // Previous state
   routing_async_state  state;        // Current state
   bool                 was_async;    // Was the last cache_get async ?
   uint32               remainder_size;
   uint32               remainder;    // remainder
   uint32               bucket;       // hash bucket
   uint32               index;        // hash index
   uint64               page_addr;    // Can be index or filter
   uint64               header_addr;  // header address in filter page
   cache_async_ctxt    *cache_ctxt;   // cache ctxt for async get
} routing_async_ctxt;

platform_status
routing_filter_add(cache            *cc,
                   routing_config   *cfg,
                   platform_heap_id  hid,
                   routing_filter   *old_filter,
                   routing_filter   *filter,
                   uint32           *new_fp_arr,
                   uint64            num_new_fingerprints,
                   uint16            value);

platform_status
routing_filter_lookup(cache          *cc,
                      routing_config *cfg,
                      routing_filter *filter,
                      const char     *key,
                      uint64         *found_values);

static inline uint16
routing_filter_get_next_value(uint64 found_values,
                              uint16 last_value)
{
   if (last_value != ROUTING_NOT_FOUND) {
      uint64 mask = (1 << last_value) - 1;
      found_values &= mask;
   }
   if (found_values == 0) {
      return ROUTING_NOT_FOUND;
   }
   return 63 - __builtin_clzll(found_values);
}

static inline bool
routing_filter_is_value_found(uint64 found_values,
                              uint16 value)
{
   return found_values & (1 << value);
}


/*
 *-----------------------------------------------------------------------------
 *
 * routing_filter_ctxt_init --
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
routing_filter_ctxt_init(routing_async_ctxt *ctxt,       // OUT
                         cache_async_ctxt   *cache_ctxt, // IN
                         routing_async_cb    cb)         // IN
{
   ctxt->state      = routing_async_state_start;
   ctxt->cb         = cb;
   ctxt->cache_ctxt = cache_ctxt;
}

cache_async_result
routing_filter_lookup_async(cache              *cc,
                            routing_config     *cfg,
                            routing_filter     *filter,
                            char               *key,
                            uint64             *found_values,
                            routing_async_ctxt *ctxt);

void
routing_filter_zap(cache          *cc,
                   routing_filter *filter);

uint32
routing_filter_estimate_unique_keys_from_count(routing_config *cfg,
                                               uint64          num_unique);

uint32
routing_filter_estimate_unique_keys(routing_filter *filter,
                                    routing_config *cfg);

uint32
routing_filter_estimate_unique_fp(cache            *cc,
                                  routing_config   *cfg,
                                  platform_heap_id  hid,
                                  routing_filter   *filter,
                                  uint64            num_filters);

// Debug functions

void
routing_filter_verify(cache          *cc,
                      routing_config *cfg,
                      routing_filter *filter,
                      uint16          value,
                      iterator       *itor);

void
routing_filter_print(cache          *cc,
                     routing_config *cfg,
                     routing_filter *filter);

#endif // __ROUTING_FILTER_H
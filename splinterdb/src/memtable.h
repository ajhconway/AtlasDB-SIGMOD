
/* **********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/


/*
 * memtable.h --
 *
 *     This file contains the interface for the splinter memtable.
 */

#ifndef __MEMTABLE_H
#define __MEMTABLE_H

#include "platform.h"
#include "cache.h"
#include "btree.h"

typedef enum memtable_state {
   MEMTABLE_STATE_READY,        // if it's the correct one, go ahead and insert
   MEMTABLE_STATE_FINALIZED,
   MEMTABLE_STATE_COMPACTED,
   MEMTABLE_STATE_COMPACTING,
   MEMTABLE_STATE_INCORPORATION_ASSIGNED,
   MEMTABLE_STATE_INCORPORATING,
   MEMTABLE_STATE_INCORPORATED,
   // one of these increments generation.  ASSERTS on 64-bit overflow
   NUM_MEMTABLE_STATES,
} memtable_state;

typedef struct memtable {
   volatile memtable_state state;
   uint64                  generation;

   uint64          root_addr;
   mini_allocator  mini;
   btree_config   *cfg;

} PLATFORM_CACHELINE_ALIGNED memtable;

static inline bool
memtable_try_transition(
   memtable *mt,
   memtable_state old_state,
   memtable_state new_state)
{
   switch (old_state) {
      case MEMTABLE_STATE_READY:
         debug_assert(new_state == MEMTABLE_STATE_FINALIZED);
         break;
      case MEMTABLE_STATE_FINALIZED:
         debug_assert(new_state == MEMTABLE_STATE_COMPACTING);
         break;
      case MEMTABLE_STATE_COMPACTING:
         debug_assert(new_state == MEMTABLE_STATE_COMPACTED);
         break;
      case MEMTABLE_STATE_COMPACTED:
         // A compacted memtable can transition to ASSIGNED when it goes
         // through the normal path where the compacting thread incorporates it
         // or it can transition straight to INCORPORATING if it is finishes
         // before the prior memtable and is incorporated by that thread.
         debug_assert(new_state == MEMTABLE_STATE_INCORPORATION_ASSIGNED);
         break;
      case MEMTABLE_STATE_INCORPORATION_ASSIGNED:
         // This occurs after the lookup lock has been acquired in
         // incorporate_memtable
         debug_assert(new_state == MEMTABLE_STATE_INCORPORATING);
         break;
      case MEMTABLE_STATE_INCORPORATING:
         // This transition happens when incorporation has completed
         debug_assert(new_state == MEMTABLE_STATE_INCORPORATED);
         break;
      case MEMTABLE_STATE_INCORPORATED:
         debug_assert(new_state == MEMTABLE_STATE_READY);
         break;
      default:
         debug_assert(0);
   }

   memtable_state actual_old_state =
      __sync_val_compare_and_swap(&mt->state, old_state, new_state);
   if (actual_old_state != old_state) {
      switch (old_state) {
         case MEMTABLE_STATE_COMPACTED:
            debug_assert(
                  actual_old_state != MEMTABLE_STATE_INCORPORATION_ASSIGNED);
            debug_assert(actual_old_state != MEMTABLE_STATE_INCORPORATING);
            break;
         default:
         platform_assert(0);
      }
   }
   return actual_old_state == old_state;
}

static inline void
memtable_transition(memtable *mt,
                    memtable_state old_state,
                    memtable_state new_state)
{
   bool success = memtable_try_transition(mt, old_state, new_state);
   platform_assert(success);
}

typedef void (*process_fn)(void *arg, uint64 generation);

typedef struct memtable_config {
   uint64        max_tuples_per_memtable;
   uint64        max_memtables;

   btree_config *btree_cfg;
} memtable_config;

typedef struct memtable_context {
   cache           *cc;
   memtable_config  cfg;

   process_fn       process;
   void            *process_ctxt;

   // Protected by insert_lock. Can read without lock. Must get read lock to
   // freeze and write lock to modify.
   uint64           insert_lock_addr;
   volatile uint64  generation;

   // Protected by incorporation_mutex. Must hold to read or modify.
   platform_mutex   incorporation_mutex;
   volatile uint64  generation_to_incorporate;

   // Protected by the lookup lock. Must hold read lock to read and write lock
   // to modify.
   uint64           lookup_lock_addr;
   volatile uint64  generation_retired;

   /*
    * num_tuples is the sum of per-thread inserted tuples, each up to the last
    * MEMTABLE_COUNT_GRANULARITY tuples (So this will be accurate up to
    * MEMTABLE_COUNT_GRANULARITY * number_of_threads).
    *
    * thread_num_tuples is the per-thread remainder modulo BNTG
    *
    * Actual number of tuples is therefore
    * num_tuples + sum_{i=0..MAX_THREADS}(thread_num_tuples[i])
    *
    * global and thread local counters use atomic instructions instad of the
    * write lock on lock page
    *    -- Need to have read_lock(lock_addr) to modify even with atomic
    *       instructions
    *    -- Need write_lock(lock_addr) to clear when rotating
    */
   uint64               num_tuples;
   cache_aligned_uint64 thread_num_tuples[MAX_THREADS];

   // Effectively thread local, no locking at all:
   btree_scratch scratch[MAX_THREADS];

   memtable mt[/*cfg.max_memtables*/];
} memtable_context;

platform_status
memtable_maybe_rotate_and_get_insert_lock(memtable_context  *ctxt,
                                          uint64            *generation,
                                          page_handle      **lock_page);

void
memtable_unget_insert_lock(memtable_context *ctxt,
                           page_handle      *lock_page);

platform_status
memtable_insert(memtable_context *ctxt,
                memtable         *mt,
                const char       *key,
                const char       *data,
                uint64           *generation);

page_handle *
memtable_get_lookup_lock(memtable_context *ctxt);

void
memtable_unget_lookup_lock(memtable_context *ctxt,
                           page_handle      *lock_page);

page_handle *
memtable_uncontended_get_claim_lock_lookup_lock(memtable_context *ctxt);

void
memtable_unlock_unclaim_unget_lookup_lock(memtable_context *ctxt,
                                          page_handle      *lock_page);

bool
memtable_dec_ref_maybe_recycle(memtable_context *ctxt,
                               memtable         *mt);

uint64
memtable_force_finalize(memtable_context *ctxt);

void
memtable_init(memtable         *mt,
              cache            *cc,
              memtable_config  *cfg,
              uint64            generation);

void
memtable_deinit(cache            *cc,
                memtable         *mt);

memtable_context *
memtable_context_create(platform_heap_id  hid,
                        cache            *cc,
                        memtable_config  *cfg,
                        process_fn        process,
                        void             *process_ctxt);

void
memtable_context_destroy(platform_heap_id  hid,
                         memtable_context *ctxt);

void
memtable_config_init(memtable_config *cfg,
                     btree_config    *btree_cfg,
                     uint64           max_memtables,
                     uint64           memtable_capacity);

static inline uint64
memtable_root_addr(memtable *mt)
{
   return mt->root_addr;
}

static inline uint64
memtable_generation(memtable_context *ctxt)
{
   return ctxt->generation;
}

static inline uint64
memtable_generation_to_incorporate(memtable_context *ctxt)
{
   return ctxt->generation_to_incorporate;
}

static inline uint64
memtable_generation_retired(memtable_context *ctxt)
{
   return ctxt->generation_retired;
}

/*
 * Must hold write lock on insert_lock
 */
static inline void
memtable_increment_to_generation_to_incorporate(memtable_context *ctxt,
                                                uint64            generation)
{
   platform_assert(ctxt->generation_to_incorporate + 1 == generation);
   // protected by mutex, so don't need atomics
   ctxt->generation_to_incorporate++;
}

/*
 * Must hold write lock on lookup_lock
 */
static inline void
memtable_increment_to_generation_retired(memtable_context *ctxt,
                                         uint64            generation)
{
   platform_assert(ctxt->generation_retired + 1 == generation);
   // protected by lookup_lock, so don't need atomics
   ctxt->generation_retired++;
}

static inline void
memtable_lock_incorporation_lock(memtable_context *ctxt)
{
   platform_mutex_lock(&ctxt->incorporation_mutex);
}

static inline void
memtable_unlock_incorporation_lock(memtable_context *ctxt)
{
   platform_mutex_unlock(&ctxt->incorporation_mutex);
}

static inline void
memtable_zap(cache    *cc,
             memtable *mt)
{
   btree_zap(cc, mt->cfg, mt->root_addr, PAGE_TYPE_MEMTABLE);
}

static inline bool
memtable_ok_to_lookup(memtable *mt)
{
   return mt->state != MEMTABLE_STATE_INCORPORATING &&
          mt->state != MEMTABLE_STATE_INCORPORATED;
}

static inline bool
memtable_ok_to_lookup_compacted(memtable *mt)
{
   return mt->state == MEMTABLE_STATE_COMPACTED ||
          mt->state == MEMTABLE_STATE_INCORPORATION_ASSIGNED;
}

bool
memtable_is_empty(memtable_context *mt_ctxt);

static inline bool
memtable_verify(cache    *cc,
                memtable *mt)
{
   return btree_verify_tree(cc, mt->cfg, mt->root_addr, PAGE_TYPE_MEMTABLE);
}

static inline void
memtable_print(cache    *cc,
               memtable *mt)
{
   btree_print_tree(cc, mt->cfg, mt->root_addr);
}

static inline void
memtable_print_stats(cache           *cc,
                     memtable        *mt)
{
   btree_print_tree_stats(cc, mt->cfg, mt->root_addr);
};

static inline void
memtable_key_to_string(memtable   *mt,
                       const char *key,
                       char       *key_str)
{
   btree_key_to_string(mt->cfg, key, key_str);
}

#endif // __MEMTABLE_H
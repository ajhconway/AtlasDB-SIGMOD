/* ***********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************
 */

/*
 * memcache.c --
 *
 *     This file contains the implementation for a dummy in-memory cache.
 */
#include "platform.h"

#include "memcache.h"
#include "util.h"

#include "poison.h"

platform_status memcache_alloc_extent (memcache *cc, page_handle **page_arr, page_type type);
bool            memcache_dealloc      (memcache *cc, uint64 addr, page_type type);
page_handle *   memcache_get          (memcache *cc, uint64 addr, bool blocking, page_type type);
void            memcache_unget        (memcache *cc, page_handle *page);
bool            memcache_claim        (memcache *cc, page_handle *page);
void            memcache_unclaim      (memcache *cc, page_handle *page);
void            memcache_lock         (memcache *cc, page_handle *page);
void            memcache_unlock       (memcache *cc, page_handle *page);

static cache_ops memcache_ops = {
   .extent_alloc      = (extent_alloc_fn)      memcache_alloc_extent,
   .page_dealloc      = (page_dealloc_fn)      memcache_dealloc,
   .page_get          = (page_get_fn)          memcache_get,
   .page_unget        = (page_unget_fn)        memcache_unget,
   .page_claim        = (page_claim_fn)        memcache_claim,
   .page_unclaim      = (page_unclaim_fn)      memcache_unclaim,
   .page_lock         = (page_lock_fn)         memcache_lock,
   .page_unlock       = (page_unlock_fn)       memcache_unlock,
};

platform_status
memcache_init(memcache         *mc,
              uint64            capacity)
{
   platform_assert(mc != NULL);
   ZERO_CONTENTS(mc);

   mc->super.ops = &memcache_ops;
   mc->page_size = 4096;
   mc->extent_size = 32 * 4096;
   mc->pages_per_extent = 32;
   platform_assert(capacity % mc->extent_size == 0);
   mc->capacity = capacity;
   mc->next = 0;

   /* data must be aligned because of O_DIRECT */
   mc->bh = platform_buffer_create(mc->capacity, 0, 0);
   if (!mc->bh) {
      goto alloc_error;
   }
   mc->data = mc->bh->addr;

   uint64 num_handles = mc->capacity / mc->page_size;
   mc->handle_bh = platform_buffer_create(num_handles, 0, 0);
   if (!mc->handle_bh) {
      goto alloc_error;
   }
   mc->handle = mc->handle_bh->addr;

   return STATUS_OK;

alloc_error:
   memcache_deinit(mc);
   return STATUS_NO_MEMORY;
}

void
memcache_deinit(memcache *mc)
{
   platform_assert(mc != NULL);

   if (mc->bh) {
      platform_buffer_destroy(mc->bh);
   }
   mc->data = NULL;

   if (mc->handle_bh) {
      platform_buffer_destroy(mc->handle_bh);
   }
   mc->handle = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * memcache_alloc_extent --
 *
 *      Allocates an extent in memory.
 *
 *----------------------------------------------------------------------
 */

platform_status
memcache_alloc_extent(memcache *   mc,
                      page_handle *page_arr[static MAX_PAGES_PER_EXTENT],
                      page_type    type)
{
   platform_assert(mc->next * mc->extent_size <= mc->capacity);

   uint64 next = __sync_fetch_and_add(&mc->next, 1);
   char *next_page = mc->data + next * mc->extent_size;
   page_handle *next_handle = mc->handle + next;
   for (uint64 i = 0; i < mc->pages_per_extent; i++) {
      next_handle->data = next_page;
      next_handle->disk_addr = (uint64)next_handle;
      next_handle++;
      next_page += mc->page_size;
   }
   return STATUS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * memcache_dealloc --
 *
 *      No op.
 *
 *----------------------------------------------------------------------
 */

bool
memcache_dealloc(memcache *cc,
                 uint64      addr,
                 page_type   type)
{
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * memcache_get --
 *
 *----------------------------------------------------------------------
 */

page_handle *
memcache_get(memcache * mc,
             uint64     addr,
             bool       blocking,
             page_type  type)
{
   return (page_handle *)addr;
}

void
memcache_unget(memcache *mc,
               page_handle *page)
{
   // no op
}


/*
 *----------------------------------------------------------------------
 *
 * memcache_claim --
 *
 *----------------------------------------------------------------------
 */

bool
memcache_claim(memcache *mc,
               page_handle *page)
{
   return TRUE;
}

void
memcache_unclaim(memcache *mc,
                   page_handle *page)
{
   // no op
}


/*
 *----------------------------------------------------------------------
 *
 * memcache_lock --
 *
 *----------------------------------------------------------------------
 */

void
memcache_lock(memcache  *mc,
              page_handle *page)
{
   // no op
}

void
memcache_unlock(memcache  *mc,
                page_handle *page)
{
   // no op
}

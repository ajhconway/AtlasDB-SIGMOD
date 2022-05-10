/* **********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * memcache.h --
 *
 *     This file contains interface for a dummy in-memory cache.
 */

#ifndef __MEMCACHE_H
#define __MEMCACHE_H

#include "cache.h"


typedef struct memcache {
   cache           super;
   uint64          page_size;
   uint64          extent_size;
   uint64          capacity;
   uint64          pages_per_extent;
   buffer_handle * bh;
   char *          data;
   uint64          next;
   buffer_handle * handle_bh;
   page_handle *   handle;
} memcache;

platform_status
memcache_init(memcache         *mc,
              uint64            capacity);

void
memcache_deinit(memcache *cc);

#endif // __MEMCACHE_H

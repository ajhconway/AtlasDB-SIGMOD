#include "allocator.h"
#include "cache.h"
#include "splinter.h"
#include "platform.h"

platform_status
test_functionality(allocator            *al,
                   io_handle            *io,
                   cache                *cc[],
                   splinter_config      *cfg,
                   uint64                seed,
                   uint64                num_inserts,
                   uint64                correctness_check_frequency,
                   task_system          *ts,
                   platform_heap_handle  hh,
                   platform_heap_id      hid,
                   uint8                 num_tables,
                   uint8                 num_caches,
                   uint32                max_async_inflight);

static inline void
functionality_key_to_string(const void *key,
                            char       *str,
                            size_t      max_len)
{
   sprintf(str, "0x%08lx", be64toh(*(uint64 *)key));
}

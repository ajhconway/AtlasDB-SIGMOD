/*************************************************************************
 * Copyright 2018-2020 VMware, Inc. All rights reserved -- VMware Confidential
 *************************************************************************/

/*
 * merge.h --
 *
 *    Merging functionality, notably merge iterators.
 */

#ifndef __MERGE_H
#define __MERGE_H

#include "btree.h"
#include "iterator.h"
#include "platform.h"

#define MAX_MERGE_ARITY (1024)

typedef struct ordered_iterator {
   iterator *itor;
   int seq;
   char *key;
   char *data;
   data_type type;
   bool next_key_equal;
} ordered_iterator;

/*
 * range_stack structure is used by merge iterator to keep track
 * of range deletes.
 * For invariants of range stack take a look at: range_stack_check_invariants
 *
 */
typedef struct range_stack {
   uint32 size;
   char   limits[MAX_MERGE_ARITY][MAX_KEY_SIZE];
   char   start_key[MAX_KEY_SIZE];
   int    end_seq;
   int    seq[MAX_MERGE_ARITY];
   int    num_sequences;
   bool   has_start_key;
} range_stack;

typedef struct merge_iterator {
   iterator               super;           // handle for iterator.h API
   int                    num_trees;       // number of trees in the forest
   bool                   discard_deletes; // Whether to emit delete messages
   bool                   resolve_updates; // Whether to merge updates with NULL
   bool                   has_data;        // Whether to look at data at all
   bool                   at_end;
   int                    num_remaining;   // number of ritors not at end
   data_config           *cfg;             // point message tree data config
   data_config           *range_cfg;       // range delete tree data config
   char                  *key;             // next key
   char                  *data;            // next data
   data_type              type;            // next data type

   // Padding so ordered_iterators[-1] is valid
   ordered_iterator       ordered_iterator_stored_pad;
   ordered_iterator       ordered_iterator_stored[MAX_MERGE_ARITY];
   // Padding so ordered_iterator_ptr_arr[-1] is valid
   // Must be immediately before ordered_iterator_ptr_arr
   ordered_iterator      *ordered_iterators_pad;
   ordered_iterator      *ordered_iterators[MAX_MERGE_ARITY];

   // Stats
   uint64                 discarded_deletes;

   char                   start_key_buffer[MAX_KEY_SIZE];
   char                   end_key_buffer[MAX_KEY_SIZE];

   range_stack stack;

   // space for merging data together
   char                   merge_buffer[MAX_DATA_SIZE];
} merge_iterator;
// Statically enforce that the padding variables act as index -1 for both arrays
_Static_assert(offsetof(merge_iterator, ordered_iterator_stored_pad) ==
               offsetof(merge_iterator, ordered_iterator_stored[-1]), "");
_Static_assert(offsetof(merge_iterator, ordered_iterators_pad) ==
               offsetof(merge_iterator, ordered_iterators[-1]), "");

platform_status
merge_iterator_create(platform_heap_id  hid,
                      data_config      *cfg,
                      data_config      *range_cfg,
                      int               num_trees,
                      iterator        **itor_arr,
                      bool              discard_deletes,
                      bool              resolve_updates,
                      bool              has_data,
                      merge_iterator  **out_itor);

platform_status
merge_iterator_destroy(platform_heap_id hid, merge_iterator **merge_itor);

static inline void
merge_iterator_print(merge_iterator *merge_itor)
{
   uint64 i;
   char *key, *data, key_str[MAX_KEY_SIZE];
   data_type type;
   data_config *data_cfg = merge_itor->cfg;
   iterator_get_curr(&merge_itor->super, &key, &data, &type);
   data_cfg->key_to_string(key, key_str, 32);

   platform_log("****************************************\n");
   platform_log("** merge iterator\n");
   platform_log("**  - trees: %u remaining: %u\n", merge_itor->num_trees, merge_itor->num_remaining);
   platform_log("** curr: %s\n", key_str);
   platform_log("----------------------------------------\n");
   for (i = 0; i < merge_itor->num_trees; i++) {
      bool at_end;
      iterator_at_end(merge_itor->ordered_iterators[i]->itor, &at_end);
      platform_log("%u: ", merge_itor->ordered_iterators[i]->seq);
      if (at_end)
         platform_log("# : ");
      else
         platform_log("_ : ");
      if (i < merge_itor->num_remaining) {
         iterator_get_curr(merge_itor->ordered_iterators[i]->itor, &key, &data, &type);
         data_cfg->key_to_string(key, key_str, 32);
         platform_log("%s\n", key_str);
      } else {
         platform_log("\n");
      }
   }
   platform_log("\n");

}

#endif // __BTREE_MERGE_H

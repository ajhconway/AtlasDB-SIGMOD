/* **********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * data.h --
 *
 *     This file contains constants and functions the pertain to
 *     keys, data and tuples.
 */

#ifndef __DATA_H
#define __DATA_H

#include "platform.h"

#define MAX_KEY_SIZE 24
#define MAX_DATA_SIZE 1026

typedef enum message_type {
   MESSAGE_TYPE_INSERT,
   MESSAGE_TYPE_UPDATE,
   MESSAGE_TYPE_DELETE,
   MESSAGE_TYPE_INVALID,
} message_type;


/*
 * Tests usually allocate a number of pivot keys.
 * Since we can't use VLAs, it's easier to allocate an array of a struct
 * than to malloc a 2d array which requires a loop of some kind (or math to
 * dereference)
 * Define a struct for a key of max size.
 */
typedef struct {
   char k[MAX_KEY_SIZE];
} key_buffer;


typedef int  (*key_compare_fn) (const void *key1, const void *key2, size_t size);
typedef void (*key_or_data_to_str_fn) (const void *key_or_data,
                                       char *str,
                                       size_t size);
typedef void (*clobber_message_with_range_delete_fn) (const void *key, const void *data);

typedef void (*merge_tuple_fn) (const void *key,
                                const void *old_raw_data,
                                void *new_raw_data);
typedef void (*merge_tuple_final_fn) (const void *key,
                                      void *oldest_raw_data);
typedef message_type (*data_class_fn) (void *raw_data);

typedef struct data_config {
   uint64         key_size;
   uint64         data_size;
   char           min_key[MAX_KEY_SIZE];
   char           max_key[MAX_KEY_SIZE];

   char           point_delete_data[MAX_DATA_SIZE];

   key_compare_fn        key_compare;
   key_or_data_to_str_fn key_to_string;
   key_or_data_to_str_fn data_to_string;
   merge_tuple_fn        merge_tuples;
   merge_tuple_final_fn  merge_tuples_final;
   data_class_fn         data_class;
   clobber_message_with_range_delete_fn clobber_message_with_range_delete;
} data_config;

#include "platform.h"

typedef struct __attribute__ ((packed)) data_handle {
   uint8 message_type;
   int8  ref_count;
   uint8 data[0];
} data_handle;

static inline int
data_key_compare(const data_config *cfg,
                 const void  *key1,
                 const void  *key2)
{
   return cfg->key_compare(key1, key2, cfg->key_size);
}

static inline void
data_key_copy(const data_config *cfg,
              void              *dst,
              const void        *src)
{
   memmove(dst, src, cfg->key_size);
}

static inline void
data_set_insert_flag(void *raw_data)
{
   data_handle *data = (data_handle *)raw_data;
   data->message_type = MESSAGE_TYPE_INSERT;
}

static inline void
data_set_delete_flag(void *raw_data)
{
   data_handle *data = (data_handle *)raw_data;
   data->message_type = MESSAGE_TYPE_DELETE;
}

static inline void
data_set_insert(void  *raw_data,
                int8   ref_count,
                char  *val,
                uint64 data_size)
{
   memset(raw_data, 0, data_size);
   data_handle *data = (data_handle *)raw_data;
   data_set_insert_flag(data);
   data->ref_count = ref_count;
   memmove(data->data, val, data_size - 2);
}

/*
 *-----------------------------------------------------------------------------
 *
 * data_merge_tuples --
 *
 *      Given two data messages, merges them by decoding the type of messages.
 *      Returns the result in new_data.
 *
 *-----------------------------------------------------------------------------
 */

static inline void
data_merge_tuples(const void *key,
                  const void *old_raw_data,
                  void *new_raw_data)
{
   const data_handle *old_data = old_raw_data;
   data_handle *new_data = new_raw_data;
   debug_assert(old_data != NULL);
   debug_assert(new_data != NULL);
   //platform_log("data_merge_tuples: op=%d old_op=%d key=0x%08lx old=%d new=%d\n",
   //         new_data->message_type, old_data->message_type, htobe64(*(uint64 *)key),
   //         old_data->ref_count, new_data->ref_count);

   switch (new_data->message_type) {
      case MESSAGE_TYPE_INSERT:
      case MESSAGE_TYPE_DELETE:
         break;
      case MESSAGE_TYPE_UPDATE:
         switch (old_data->message_type) {
            case MESSAGE_TYPE_INSERT:
               new_data->message_type = MESSAGE_TYPE_INSERT;
               new_data->ref_count += old_data->ref_count;
               break;
            case MESSAGE_TYPE_UPDATE:
               new_data->ref_count += old_data->ref_count;
               break;
            case MESSAGE_TYPE_DELETE:
               if (new_data->ref_count == 0) {
                  new_data->message_type = MESSAGE_TYPE_DELETE;
               } else  {
                  new_data->message_type = MESSAGE_TYPE_INSERT;
               }
               break;
            default:
               platform_assert(0);
         }
         break;
      default:
         platform_assert(0);
   }

   //if (new_data->message_type == MESSAGE_TYPE_INSERT) {
   //   ;
   //} else if (new_data->message_type == MESSAGE_TYPE_DELETE) {
   //   ;
   //} else if (old_data == NULL || old_data->message_type == MESSAGE_TYPE_DELETE) {
   //   if (new_data->ref_count == 0)
   //      new_data->message_type = MESSAGE_TYPE_DELETE;
   //   else
   //      new_data->message_type = MESSAGE_TYPE_INSERT;
   //} else if (old_data->message_type == MESSAGE_TYPE_INSERT) {
   //   new_data->message_type = MESSAGE_TYPE_INSERT;
   //   new_data->ref_count += old_data->ref_count;
   //} else {
   //   new_data->ref_count += old_data->ref_count;
   //}
}

/*
 *-----------------------------------------------------------------------------
 *
 * data_merge_tuples_final --
 *
 *      Called for non-MESSAGE_TYPE_INSERT messages when they are determined to be the oldest
 *      message in the system.
 *
 *      Can change data_class or contents.  If necessary, update new_data.
 *
 *-----------------------------------------------------------------------------
 */
static inline void
data_merge_tuples_final(
                        const void *key,       // IN
                        void *oldest_raw_data) // IN/OUT
{
   data_handle *old_data = oldest_raw_data;
   debug_assert(old_data != NULL);

   if (old_data->message_type == MESSAGE_TYPE_UPDATE) {
      old_data->message_type = (old_data->ref_count == 0)
                               ? MESSAGE_TYPE_DELETE
                               : MESSAGE_TYPE_INSERT;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * data_class --
 *
 *      Given a data message, returns its message class.
 *
 *-----------------------------------------------------------------------------
 */

static inline message_type
data_class(void *raw_data)
{
   data_handle *data = raw_data;
   switch(data->message_type) {
      case MESSAGE_TYPE_INSERT:
         return data->ref_count == 0 ? MESSAGE_TYPE_DELETE : MESSAGE_TYPE_INSERT;
      case MESSAGE_TYPE_DELETE:
         return MESSAGE_TYPE_DELETE;
      case MESSAGE_TYPE_UPDATE:
         return MESSAGE_TYPE_UPDATE;
      default:
         platform_error_log("data class error: %d\n", data->message_type);
         platform_assert(0);
   }
   return MESSAGE_TYPE_INVALID;
}

static inline void
data_key_to_string(const void *key,
                   char       *str,
                   size_t      len)
{
   //snprintf(str, 24, "%s", key);
   if (len == 24) {
      const uint64 *key_p = key;
      //sprintf(str, "0x%016lx%016lx%016lx", be64toh(key_p[0]),
      //        key_p[1], key_p[2]);
      sprintf(str, "0x%016lx", be64toh(key_p[0]));
   }
   if (len == 8) {
      const uint64 *key_p = key;
      sprintf(str, "0x%016lx", be64toh(key_p[0]));
   }
}

static inline void
data_data_to_string(const void *raw_data,
                    char       *str_p,
                    size_t      max_len)
{
   const data_handle *data = raw_data;
   sprintf(str_p, "%d:%d:%lu", data->message_type, data->ref_count,
          *(uint64 *)data->data);
   //for (uint64 i = 0; i < max_len - 2; i++)
   //   sprintf(&str_p[4 + 2*i], "%02x", data->data[i]);
   //memmove(str_p + 4, data->data, max_len - 2);
}

static inline void
data_print_key(const void *key)
{
   const uint64 *key_p = key;
   platform_log("0x%016lx%016lx%016lx", be64toh(key_p[0]), key_p[1], key_p[2]);
}

#endif // __DATA_H

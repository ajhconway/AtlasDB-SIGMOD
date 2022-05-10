/* **********************************************************
 * Copyright 2018-2020 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * config.h --
 *
 *     This file contains functions for config parsing.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "clockcache.h"
#include "data.h"
#include "io.h"
#include "rc_allocator.h"
#include "shard_log.h"
#include "splinter.h"
#include "util.h"

typedef struct master_config {
   uint64 page_size;
   uint64 extent_size;

   // io
   char   io_filename[MAX_STRING_LENGTH];
   int    io_flags;
   uint32 io_perms;
   uint64 io_async_queue_depth;

   // allocator
   uint64 allocator_capacity;

   // cache
   uint64 cache_capacity;
   bool   cache_use_stats;
   char   cache_logfile[MAX_STRING_LENGTH];

   // btree
   uint64 btree_rough_count_height;

   // filter
   uint64 filter_remainder_size;
   uint64 filter_index_size;

   // log
   bool use_log;

   // splinter
   uint64 memtable_capacity;
   uint64 fanout;
   uint64 max_branches_per_node;
   uint64 use_stats;

   // data
   uint64 key_size;
   uint64 data_size;

   // test
   uint64 seed;
} master_config;

static inline void
config_set_defaults(master_config *cfg)
{
   *cfg = (master_config) {
      .io_filename = "db",
      .cache_logfile = "cache_log",

      .page_size                = 4096,
      .extent_size              = 128 * 1024,

      .io_flags                 = O_RDWR | O_CREAT,
      .io_perms                 = 0755,
      .io_async_queue_depth     = 256,

      .allocator_capacity       = GiB_TO_B(30),

      .cache_capacity           = GiB_TO_B(1),

      .btree_rough_count_height = 1,

      .filter_remainder_size    = 8,
      .filter_index_size        = 256,

      .use_log                  = FALSE,

      .memtable_capacity        = MiB_TO_B(24),
      .fanout                   = 8,
      .max_branches_per_node    = 24,
      .use_stats                = FALSE,

      .key_size                 = 24,
      .data_size                = 100,

      .seed                     = 0,
   };
}

static inline void
config_usage()
{
   platform_error_log("Configuration:\n");
   platform_error_log("\t--page_size\n");
   platform_error_log("\t--extent_size\n");
   platform_error_log("\t--db-location\n");
   platform_error_log("\t--set-O_DIRECT\n");
   platform_error_log("\t--unset-O_DIRECT\n");
   platform_error_log("\t--set-O_CREAT\n");
   platform_error_log("\t--unset-O_CREAT\n");
   platform_error_log("\t--db-perms\n");
   platform_error_log("\t--db-capacity-gib\n");
   platform_error_log("\t--db-capacity-mib\n");
   platform_error_log("\t--libaio-queue-depth\n");
   platform_error_log("\t--cache-capacity-gib\n");
   platform_error_log("\t--cache-capacity-mib\n");
   platform_error_log("\t--cache-debug-log\n");
   platform_error_log("\t--memtable-capacity-gib\n");
   platform_error_log("\t--memtable-capacity-mib\n");
   platform_error_log("\t--rough-count-height\n");
   platform_error_log("\t--filter-remainder-size\n");
   platform_error_log("\t--fanout\n");
   platform_error_log("\t--max-branches-per-node\n");
   platform_error_log("\t--stats\n");
   platform_error_log("\t--no-stats\n");
   platform_error_log("\t--log\n");
   platform_error_log("\t--no-log\n");
   platform_error_log("\t--key-size\n");
   platform_error_log("\t--data-size\n");
   platform_error_log("\t--seed\n");
}

/*
 * Config option parsing macros
 *
 * They are meant to be used inside config_parse and test_config_parse.
 * They can be used as regular conditions, and the code block after will be
 * executed when the condition satisfies.
 *
 * So config_set_*(...) { stmt; } gets expanded to:
 *
 *    } else if (...) {
 *       ...boilerplate parsing code...
 *       {
 *         stmt;
 *       }
 *    }
 *
 * config_set_string, config_set_uint* and config_set_*ib can be used for
 * parsing a single value or comma-separated multiple values.
 * When parsing a single value, each cfg will receive the same parsed value.
 * When parsing multiple values, if number of tokens doesn't match num_config,
 * will print error msg and fail.
 */

#define config_has_option(str) \
   } else if (STRING_EQUALS_LITERAL(argv[i], "--"str)) {

#define config_set_string(name, var, field)                                \
   config_has_option(name)                                                 \
   if (i + 1 == argc) {                                                    \
      platform_error_log("config: failed to parse %s\n", name);            \
      return STATUS_BAD_PARAM;                                             \
   }                                                                       \
   uint8 _idx;                                                             \
   platform_strtok_ctx _ctx = { .token_str = NULL,                         \
                                .last_token = NULL,                        \
                                .last_token_len = 0 };                     \
                                                                           \
   if (strchr(argv[++i], ',')) {                                           \
      char *_token = platform_strtok_r(argv[i], ",", &_ctx);               \
      for (_idx = 0; _token != NULL; _idx++) {                             \
         if (_idx > num_config - 1) {                                      \
            platform_error_log("config: more %s than num_tables\n", name); \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
         int _rc = snprintf(var[_idx].field, MAX_STRING_LENGTH, "%s",      \
                            _token);                                       \
         if (_rc >= MAX_STRING_LENGTH) {                                   \
            platform_error_log("config: %s too long\n", name);             \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
                                                                           \
         _token = platform_strtok_r(NULL, ",", &_ctx);                     \
      }                                                                    \
      if (_idx < num_config) {                                             \
         platform_error_log("config: less %s than num_tables\n", name);    \
         return STATUS_BAD_PARAM;                                          \
      }                                                                    \
   } else {                                                                \
      for (_idx = 0; _idx < num_config; _idx++) {                          \
         int _rc = snprintf(var[_idx].field, MAX_STRING_LENGTH, "%s",      \
                            argv[i]);                                      \
         if (_rc >= MAX_STRING_LENGTH) {                                   \
            platform_error_log("config: %s too long\n", name);             \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
      }                                                                    \
   }

#define _config_set_numerical(name, var, field, type)                      \
   config_has_option(name)                                                 \
   if (i + 1 == argc) {                                                    \
      platform_error_log("config: failed to parse %s\n", name);            \
      return STATUS_BAD_PARAM;                                             \
   }                                                                       \
   uint8 _idx;                                                             \
   platform_strtok_ctx _ctx = { .token_str = NULL, .last_token = NULL,     \
                                .last_token_len = 0 };                     \
                                                                           \
   if (strchr(argv[++i], ',')) {                                           \
      char *_token = platform_strtok_r(argv[i], ",", &_ctx);               \
      for (_idx = 0; _token != NULL; _idx++) {                             \
         if (_idx > num_config - 1) {                                      \
            platform_error_log("config: more %s than num_tables\n", name); \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
         if (!try_string_to_##type(_token, &var[_idx].field)) {            \
            platform_error_log("config: failed to parse %s\n", name);      \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
                                                                           \
         _token = platform_strtok_r(NULL, ",", &_ctx);                     \
      }                                                                    \
      if (_idx < num_config) {                                             \
         platform_error_log("config: less %s than num_tables\n", name);    \
         return STATUS_BAD_PARAM;                                          \
      }                                                                    \
   } else {                                                                \
      for (_idx = 0; _idx < num_config; _idx++) {                          \
         if (!try_string_to_##type(argv[i], &var[_idx].field)) {           \
            platform_error_log("config: failed to parse %s\n", name);      \
            return STATUS_BAD_PARAM;                                       \
         }                                                                 \
      }                                                                    \
   }

#define config_set_uint8(name, var, field) \
   _config_set_numerical(name, var, field, uint8)

#define config_set_uint32(name, var, field) \
   _config_set_numerical(name, var, field, uint32)

#define config_set_uint64(name, var, field) \
   _config_set_numerical(name, var, field, uint64)

#define config_set_mib(name, var, field)          \
   config_set_uint64(name"-mib", var, field) {    \
      for (uint8 _i = 0; _i < num_config; _i++) { \
         var[_i].field = MiB_TO_B(var[_i].field); \
      }                                           \
   }

#define config_set_gib(name, var, field)          \
   config_set_uint64(name"-gib", var, field) {    \
      for (uint8 _i = 0; _i < num_config; _i++) { \
         var[_i].field = GiB_TO_B(var[_i].field); \
      }                                           \
   }

#define config_set_else } else

static inline platform_status
config_parse(master_config *cfg,
             const uint8    num_config,
             int            argc,
             char          *argv[])
{
   uint64 i;
   uint8 cfg_idx;
   for (i = 0; i < argc; i++) {
      if (0) {
      config_set_uint64("page-size", cfg, page_size) {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            if (cfg[cfg_idx].page_size != 4096) {
               platform_error_log("page_size must be 4096 for now\n");
               platform_error_log("config: failed to parse page-size\n");
               return STATUS_BAD_PARAM;
            }
            if (!IS_POWER_OF_2(cfg[cfg_idx].page_size)) {
               platform_error_log("page_size must be a power of 2\n");
               platform_error_log("config: failed to parse page-size\n");
               return STATUS_BAD_PARAM;
            }
         }
      } config_set_uint64("extent-size", cfg, extent_size) {
      } config_set_string("db-location", cfg, io_filename) {
      } config_has_option("set-O_DIRECT") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].io_flags |= O_DIRECT;
         }
      } config_has_option("unset-O_DIRECT") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].io_flags &= ~O_DIRECT;
         }
      } config_has_option("set-O_CREAT") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].io_flags |= O_CREAT;
         }
      } config_has_option("unset-O_CREAT") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].io_flags &= ~O_CREAT;
         }
      } config_set_uint32("db-perms", cfg, io_perms) {
      } config_set_mib("db-capacity", cfg, allocator_capacity) {
      } config_set_gib("db-capacity", cfg, allocator_capacity) {
      } config_set_uint64("libaio-queue-depth", cfg, io_async_queue_depth) {
      } config_set_mib("cache-capacity", cfg, cache_capacity) {
      } config_set_gib("cache-capacity", cfg, cache_capacity) {
      } config_set_string("cache-debug-log", cfg, cache_logfile) {
      } config_set_mib("memtable-capacity", cfg, memtable_capacity) {
      } config_set_gib("memtable-capacity", cfg, memtable_capacity) {
      } config_set_uint64("rough-count-height", cfg, btree_rough_count_height) {
      } config_set_uint64("filter-remainder-size", cfg, filter_remainder_size) {
      } config_set_uint64("fanout", cfg, fanout) {
      } config_set_uint64("max-branches-per-node", cfg, max_branches_per_node) {
      } config_has_option("stats") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].use_stats = TRUE;
         }
      } config_has_option("no-stats") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].use_stats = FALSE;
         }
      } config_has_option("log") {
         for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
            cfg[cfg_idx].use_log = TRUE;
         }
      } config_set_uint64("key-size", cfg, key_size) {
      } config_set_uint64("data-size", cfg, data_size) {
      } config_set_uint64("seed", cfg, seed) {
      } config_set_else {
         platform_error_log("config: invalid option: %s\n", argv[i]);
         return STATUS_BAD_PARAM;
      }
   }
   for (cfg_idx = 0; cfg_idx < num_config; cfg_idx++) {
      if (cfg[cfg_idx].extent_size % cfg[cfg_idx].page_size != 0) {
         platform_error_log("config: extent_size is not a multiple of page_size\n");
         return STATUS_BAD_PARAM;
      }
      if (cfg[cfg_idx].extent_size / cfg[cfg_idx].page_size > MAX_PAGES_PER_EXTENT) {
         platform_error_log("config: pages per extent too high: %lu > %lu\n",
                            cfg[cfg_idx].extent_size / cfg[cfg_idx].page_size,
                            MAX_PAGES_PER_EXTENT);
         return STATUS_BAD_PARAM;
      }
   }

   return STATUS_OK;
}

#endif

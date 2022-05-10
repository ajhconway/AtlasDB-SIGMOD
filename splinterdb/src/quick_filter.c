#include "platform.h"

#include "quick_filter.h"
#include "iterator.h"

#include "poison.h"

quick_filter_params
quick_compute_params(quick_filter_config *cfg,
                     uint64               num_fingerprints)
{
   quick_filter_params params;
   memset(&params, 0, sizeof(params));
   params.num_fingerprints = num_fingerprints;
   params.remainder_mask = (1 << cfg->remainder_size) - 1;

   params.num_buckets = 1;
   while(params.num_buckets < num_fingerprints) {
      params.num_buckets *= 2;
      params.bucket_size++;
   }
   while(params.num_buckets < cfg->index_size) {
      params.num_buckets *= 2;
      params.bucket_size++;
   }

   return params;
}

// A 4x256 matrix is used for RadixSort
#define MATRIX_ROWS sizeof(uint32)
#define MATRIX_COLS (UINT8_MAX + 1)

// XXX Change arguments to struct
static uint32 *
RadixSort(uint32 *pData,
          uint32  mBuf[static MATRIX_ROWS * MATRIX_COLS],
          uint32 *pTemp,
          uint32  count,
          uint32  bucketlen,
          uint32  remainder_size)
{
   uint32 *mIndex[MATRIX_ROWS];       // index matrix
   uint32 *pDst, *pSrc, *pTmp;
   uint32 i,j,m,n;
   uint32 u;
   uint32 fpover = remainder_size % 8;
   if (bucketlen == 0)
      bucketlen = 1;
   uint32 rounds = (bucketlen + fpover - 1) / 8 + 1;
   uint8 c;
   uint32 fpshift = remainder_size / 8;
   remainder_size = remainder_size / 8 * 8;

   for (i = 0; i<MATRIX_ROWS; i++) {
      mIndex[i] = &mBuf[i * MATRIX_COLS];
   }
   for(i = 0; i < count; i++){         // generate histograms
      u = pData[i] >> remainder_size;
      for(j = 0; j < rounds; j++){
         c = ((uint8 *)&u)[j];
         mIndex[j][c]++;
      }
   }

   for(j = 0; j < rounds; j++){             // convert to indices
      n = 0;
      for(i = 0; i < MATRIX_COLS; i++){
          m = mIndex[j][i];
          mIndex[j][i] = n;
          platform_assert(mIndex[j][i] <= count);
          n += m;
      }
   }

   pDst = pTemp;                       // radix sort
   pSrc = pData;
   for(j = 0; j < rounds; j++){
      for(i = 0; i < count; i++){
          u = pSrc[i];
          c = ((uint8 *)&u)[j + fpshift];
          pDst[mIndex[j][c]++] = u;
          __builtin_prefetch(&pDst[mIndex[j][c]], 1, 0);
      }
      pTmp = pSrc;
      pSrc = pDst;
      pDst = pTmp;
   }

   return(pSrc);
}

typedef struct quick_header {
   uint8 length;
   char header[0];
} quick_header;

static inline void
quick_set_bit(char *data,
              uint64 bitnum)
{
   *(data + bitnum / 8) |= (char)(1 << (bitnum % 8));
}

static inline uint64
quick_get_bucket_num(quick_filter_config *cfg,
                     uint32               fp)
{
   return fp >> cfg->remainder_size;
}

static inline uint64
quick_get_index(quick_filter_config *cfg,
                quick_filter_params *params,
                uint32               fp)
{
   return (fp >> cfg->remainder_size) / cfg->index_size;
}

static inline void
quick_add_remainder(quick_filter_config *cfg,
                    quick_filter_params *params,
                    char                *data,
                    uint32               pos,
                    uint32               fp)
{
   uint32 *window = (uint32 *)(data + (pos * cfg->remainder_size) / 8);
   uint32 shifted_fp = (fp & params->remainder_mask) << (pos * cfg->remainder_size % 8);
   *window |= shifted_fp;
}

static inline uint32
quick_get_remainder(quick_filter_config *cfg,
                    quick_filter_params *params,
                    char                *data,
                    uint32               pos)
{
   uint32 *window = (uint32 *)(data + (pos * cfg->remainder_size) / 8);
   return (*window >> (pos * cfg->remainder_size % 8)) & params->remainder_mask;
}

static inline quick_header *
quick_get_header(cache               *cc,
                 quick_filter_config *cfg,
                 uint64               filter_addr,
                 uint64               index,
                 page_handle        **filter_page)
{
   const uint64 addrs_per_page = cfg->addrs_per_page;
   debug_assert(index / addrs_per_page < 32);
   uint64 index_addr = filter_addr + cfg->page_size * (index / addrs_per_page);
   page_handle *index_page = cache_get(cc, index_addr, TRUE, PAGE_TYPE_FILTER);
   uint64 header_addr = ((uint64 *)index_page->data)[index % addrs_per_page];
   if (header_addr == 0)
      platform_log("bad header_addr: filter_addr %lu index %lu\n", filter_addr, index);
   platform_assert(header_addr != 0);
   *filter_page = cache_get(cc, header_addr - (header_addr % cfg->page_size),
                            TRUE, PAGE_TYPE_FILTER);
   quick_header *qh = (quick_header *)
      ((*filter_page)->data + (header_addr % cfg->page_size));
   cache_unget(cc, index_page);
   return qh;
}

static inline void
quick_unget_header(cache       *cc,
                   page_handle *header_page)
{
   cache_unget(cc, header_page);
}


platform_status
quick_filter_init(cache               *cc,
                  quick_filter_config *cfg,
                  platform_heap_id     hid,
                  uint64               num_fingerprints,
                  uint32              *fingerprint_arr,
                  uint64              *filter_addr)
{
   quick_filter_params params = quick_compute_params(cfg, num_fingerprints);

   uint64 num_indices = params.num_buckets / cfg->index_size;

   /*
    * temp and fingerprint_arr are used to radix sort the fps
    * count is the # of fps in each bucket and index_count is # fps
    * in each index
    */
   uint32 *count;
   uint32 *index_count;
   uint32 *matrix;
   uint32 *temp = TYPED_ARRAY_MALLOC(
      hid, temp,
      (
         num_fingerprints +                // temp
         params.num_buckets +              // count
         num_indices +                     // index_count
         MATRIX_ROWS * MATRIX_COLS         // matrix
       ));
   if (temp == NULL) {
      return STATUS_NO_MEMORY;
   }
   count = temp + num_fingerprints;
   index_count = count + params.num_buckets;
   matrix = index_count + num_indices;
   // Skip the temp array for memset, it's filled in by RadixSort()
   memset(count, 0, (params.num_buckets + num_indices +
                     MATRIX_ROWS * MATRIX_COLS) * sizeof(*count));

   // these are used to iterate through the fps
   uint32       *fp;                   // the sorted array of fingerprints
   uint64        index;                // the current index
   uint64        index_start;          // the index of the first fp in the index
   uint64        bucket;               // the current bucket
   quick_header *header;               // the current header
   uint64        header_size;          // size of the current header
   uint64        header_bit;           // the index of the current bit of the header
   uint64        remainder_block_size; // size of the current block of remainders
   uint64        i = 0;                // the index of the current fingerprint in fp

   // set up all the paging stuff
   const uint64 addrs_per_page = cfg->addrs_per_page;
   page_handle *index_page[MAX_PAGES_PER_EXTENT];
   platform_status rc = cache_extent_alloc(cc, index_page, PAGE_TYPE_FILTER);
   platform_assert_status_ok(rc);
   uint64 index_extent_base_addr = index_page[0]->disk_addr;
   *filter_addr = index_extent_base_addr;
   page_handle *filter_page[MAX_PAGES_PER_EXTENT];
   rc = cache_extent_alloc(cc, filter_page, PAGE_TYPE_FILTER);
   platform_assert_status_ok(rc);
   // we use the first filter page to store the base extent addrs
   // this lets us use all the space in the index_pages which wants
   // to be a power of 2
   page_handle *addr_page = filter_page[0];
   uint64 *filter_page_addr = (uint64 *)addr_page->data;
   uint64 *filter_extent_no = filter_page_addr;
   *filter_extent_no = 0;
   uint64 filter_page_no = 1;
   filter_page_addr++;
   filter_page_addr[(*filter_extent_no)++] = filter_page[0]->disk_addr;
   uint64 bytes_remaining_on_page = cfg->page_size;
   char *filter_cursor = filter_page[1]->data;
   memset(filter_cursor, 0, cfg->page_size);

   for (i = 0; i < num_fingerprints; i++)
      fingerprint_arr[i] >>= 32 - cfg->remainder_size - params.bucket_size;

   fp = RadixSort(fingerprint_arr, matrix, temp, num_fingerprints,
         params.bucket_size, cfg->remainder_size);

   i = 0;
   for (index = 0; index < num_indices; index++) {
      uint64 index_start = i;
      for (; i < num_fingerprints &&
            quick_get_index(cfg, &params, fp[i]) == index; i++) {
         count[quick_get_bucket_num(cfg, fp[i])]++;
      }
      index_count[index] = i - index_start;
   }

   i = 0;
   for (index = 0; index < num_indices; index++) {
      // the extra +1 is for the length byte
      header_size = (index_count[index] + cfg->index_size - 1) / 8 + 2;
      remainder_block_size =
        (0 < index_count[index] * cfg->remainder_size ? (index_count[index] * cfg->remainder_size - 1) : 0) / 8 + 4;

      if (header_size + remainder_block_size > bytes_remaining_on_page) {
         cache_unlock(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
         cache_unclaim(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
         cache_unget(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
         (filter_page_no)++;

         if (filter_page_no % (cfg->extent_size / cfg->page_size) == 0) {
            rc = cache_extent_alloc(cc, filter_page, PAGE_TYPE_FILTER);
            platform_assert_status_ok(rc);
            filter_page_addr[(*filter_extent_no)++] = filter_page[0]->disk_addr;
         }

         bytes_remaining_on_page = cfg->page_size;
         filter_cursor = filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]->data;
         memset(filter_cursor, 0, cfg->page_size);
      }


      // for now the indices must fit in a single extent
      debug_assert(index / addrs_per_page < cfg->extent_size / cfg->page_size);
      uint64 filter_page_offset =
         filter_cursor - filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]->data;
      ((uint64 *)index_page[index / addrs_per_page]->data)[index % addrs_per_page]
         = filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]->disk_addr
         + filter_page_offset;

      //platform_log("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
      //platform_log("&&&   Index 0x%lx\n", index);
      //platform_log("&&&   index_page_no %lu\n", index / addrs_per_page);
      //platform_log("&&&   index_page_addr %lu\n", index_page[index / addrs_per_page]->disk_addr);
      //platform_log("&&&   index_page_offset %lu\n", index % addrs_per_page);
      //platform_log("&&&   addr: %lu\n",
      //      filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]->disk_addr
      //   + filter_page_offset);

      header = (quick_header *)filter_cursor;
      header->length = header_size;
      header_bit = 0;
      for (bucket = index * cfg->index_size;
            bucket < ((index + 1) * cfg->index_size) && bucket <  params.num_buckets;
            bucket++) {
         header_bit += count[bucket];
         quick_set_bit(header->header, header_bit++);
      }
      filter_cursor += header_size;

      index_start = i;
      for (; i < num_fingerprints &&
            quick_get_index(cfg, &params, fp[i]) == index; i++) {
         quick_add_remainder(cfg, &params, filter_cursor, i - index_start, fp[i]);
         //platform_log("bucket_offset: 0x%x remainder: 0x%x\n",
         //      quick_get_bucket_num(cfg, fp[i]), fp[i] & params.remainder_mask);
      }
      filter_cursor += remainder_block_size;
      debug_assert(bytes_remaining_on_page >= header_size + remainder_block_size);
      bytes_remaining_on_page -= header_size + remainder_block_size;
   }

   do {
      cache_unlock(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
      cache_unclaim(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
      cache_unget(cc, filter_page[filter_page_no % (cfg->extent_size / cfg->page_size)]);
      (filter_page_no)++;
   } while (filter_page_no % (cfg->extent_size / cfg->page_size) != 0);

   for (i = 0; i < cfg->extent_size / cfg->page_size; i++) {
      cache_unlock(cc, index_page[i]);
      cache_unclaim(cc, index_page[i]);
      cache_unget(cc, index_page[i]);
   }

   cache_unlock(cc, addr_page);
   cache_unclaim(cc, addr_page);
   cache_unget(cc, addr_page);
   platform_free(hid, temp);

   return rc;
}

void
quick_filter_print_encoding(quick_header *qh)
{
   uint32 i;

   platform_open_log_stream();
   platform_log_stream("--- Encoding: %u\n", qh->length);
   for (i = 0; i < qh->length * 8; i++) {
      if (i != 0 && i % 16 == 0)
         platform_log_stream(" | ");
      if (qh->header[i / 8] & (1 << i % 8))
         platform_log_stream("1");
      else
         platform_log_stream("0");
   }
   platform_log_stream("\n");
   platform_close_log_stream(PLATFORM_DEFAULT_LOG_HANDLE);
}


static inline void
quick_get_bucket_bounds(char *encoding,
                        uint64 len,
                        uint64 bucket_offset,
                        uint64 *start,
                        uint64 *end)
{
   uint32 word = 0;
   uint32 encoding_word = 0;
   uint64 bucket = 0;
   uint64 bucket_pop = 0;
   uint64 bit_offset = 0;

   if (bucket_offset == 0) {
      *start = 0;
      word = 0;
      encoding_word = *((uint32 *)encoding + word);
      while (encoding_word == 0) {
         word++;
         encoding_word = *((uint32 *)encoding + word);
      }

      // ffs returns the index + 1
      bit_offset = __builtin_ffs(encoding_word) - 1;
      *end = 32 * word + bit_offset;
   } else {
      bucket_pop = platform_popcount(*((uint32 *)encoding));
      while (4 * word < len && bucket + bucket_pop < bucket_offset) {
         bucket += bucket_pop;
         word++;
         bucket_pop = platform_popcount(*((uint32 *)encoding + word));
      }

      encoding_word = *((uint32 *)encoding + word);
      while (bucket < bucket_offset - 1) {
         encoding_word &= encoding_word - 1;
         bucket++;
      }
      bit_offset = __builtin_ffs(encoding_word) - 1; // ffs returns the index + 1
      if (32 * word + bit_offset - bucket_offset + 1 >= 4096)
         platform_log("word %u bucket_offset %lu bit_offset %lu\n",
               word, bucket_offset, bit_offset);
      *start = 32 * word + bit_offset - bucket_offset + 1;

      encoding_word &= encoding_word - 1;
      while (encoding_word == 0) {
         word++;
         encoding_word = *((uint32 *)encoding + word);
      }
      bit_offset = __builtin_ffs(encoding_word) - 1; // ffs returns the index + 1
      *end = 32 * word + bit_offset - bucket_offset;
   }
}

void
quick_filter_print_remainders(quick_filter_config *cfg,
                              quick_filter_params *params,
                              quick_header *qh)
{
   uint64 i, j, start, end;

   platform_open_log_stream();
   platform_log_stream("--- Remainders\n");
   for (i = 0; i < cfg->index_size; i++) {
      quick_get_bucket_bounds(qh->header, qh->length, i, &start, &end);
      platform_log_stream("--- Bucket_offset: 0x%lx remainders: ", i);
      for (j = start; j < end; j++) {
         platform_log("0x%x ", quick_get_remainder(cfg, params, (char *)qh + qh->length, j));
      }
      platform_log_stream("\n");
   }
   platform_close_log_stream(PLATFORM_DEFAULT_LOG_HANDLE);
}

void
quick_filter_print_index(cache               *cc,
                         quick_filter_config *cfg,
                         uint64               num_fingerprints,
                         uint64               filter_addr)
{
   quick_filter_params params = quick_compute_params(cfg, num_fingerprints);
   uint64 i;
   platform_log("********************************************************************************\n");
   platform_log("***   FILTER INDEX\n");
   platform_log("***   filter_addr: %lu\n", filter_addr);
   platform_log("***   num_indices: %lu\n", params.num_buckets / cfg->index_size);
   platform_log("--------------------------------------------------------------------------------\n");
   for (i = 0; i < params.num_buckets / cfg->index_size; i++) {
      const uint64 addrs_per_page = cfg->addrs_per_page;
      uint64 index_addr = filter_addr + cfg->page_size * (i / addrs_per_page);
      page_handle *index_page = cache_get(cc, index_addr, TRUE,
            PAGE_TYPE_FILTER);
      platform_log("index 0x%lx: %lu\n", i,
            ((uint64 *)index_page->data)[i % addrs_per_page]);
      cache_unget(cc, index_page);
   }
}

void
quick_filter_print(cache               *cc,
                   quick_filter_config *cfg,
                   uint64               num_fingerprints,
                   uint64               filter_addr)
{
   quick_filter_params params = quick_compute_params(cfg, num_fingerprints);
   quick_filter_print_index(cc, cfg, num_fingerprints, filter_addr);
   uint64 i;
   for (i = 0; i < params.num_buckets / cfg->index_size; i++) {
      platform_log("--------------------------------------------------------------------------------\n");
      platform_log("--- Index 0x%lx\n", i);
      page_handle *filter_page;
      quick_header *qh = quick_get_header(cc, cfg, filter_addr, i, &filter_page);
      quick_filter_print_encoding(qh);
      quick_filter_print_remainders(cfg, &params, qh);
      quick_unget_header(cc, filter_page);
   }
}

uint64 quick_filter_num_pages(cache               *cc,
                              quick_filter_config *cfg,
                              uint64               num_fingerprints,
                              uint64               filter_addr)
{
  quick_filter_params params = quick_compute_params(cfg, num_fingerprints);

  // Count all the pages in the index
  uint64 num_pages = params.num_buckets / cfg->index_size / cfg->addrs_per_page;

  // Count the filter pages
  uint64 last_addr = 0;
  for (uint64 i = 0; i < params.num_buckets / cfg->index_size; i++) {
    page_handle *filter_page;
    quick_get_header(cc, cfg, filter_addr, i, &filter_page);
    if (filter_page->disk_addr != last_addr) {
      num_pages++;
      last_addr = filter_page->disk_addr;
    }
    quick_unget_header(cc, filter_page);
  }

  return num_pages;
}

static void
quick_compute_params_for_key(quick_filter_config *cfg,              // IN
                             uint64               num_fingerprints, // IN
                             char                *key,              // IN
                             quick_filter_params *params,           // OUT
                             uint64              *bucket,           // OUT
                             uint64              *index,            // OUT
                             uint32              *remainder)        // OUT
{
   *params = quick_compute_params(cfg, num_fingerprints);
   uint32 fp = cfg->hash(key, cfg->data_cfg->key_size, cfg->seed);

   fp >>= 32 - cfg->remainder_size - params->bucket_size;
   *bucket = quick_get_bucket_num(cfg, fp);
   *index = quick_get_index(cfg, params, fp);
   *remainder = fp & params->remainder_mask;
}

// If any change is made here, please change quick_filter_lookup_async too
int
quick_filter_lookup(cache *cc,
                    quick_filter_config *cfg,
                    uint64 filter_addr,
                    uint64 num_fingerprints,
                    char *key)
{
   uint32        remainder;
   uint64        bucket, index, start, end, i;
   quick_header *qh;
   char         *remainder_block_start;
   page_handle  *filter_node;
   int           rc = 0;

   platform_assert(filter_addr != 0);
   quick_filter_params params;

   quick_compute_params_for_key(cfg, num_fingerprints, key,
                                &params, &bucket, &index, &remainder);
   //platform_log("quick_filter_lookup: fp 0x%x index 0x%x bucket 0x%x remainder 0x%x\n",
   //      fp, index, bucket, remainder);

   qh = quick_get_header(cc, cfg, filter_addr, index, &filter_node);
   quick_get_bucket_bounds(qh->header, qh->length,
         bucket % cfg->index_size, &start, &end);
   if (start >= 4096) {
      platform_log("start %lu\n", start);
      platform_log("filter addr %lu\n", filter_addr);
      platform_log("bucket %lu index %lu\n", bucket, index);
      platform_log("max_buckets %lu max_indices %lu\n",
            params.num_buckets, params.num_buckets / cfg->index_size);
      platform_log("num_fingerprints %lu\n", num_fingerprints);
      platform_assert(0);
   }
   debug_assert(start < 4096);
   if (end >= 4096) {
      platform_log("end %lu\n", end);
      platform_log("filter addr %lu\n", filter_addr);
      platform_assert(0);
   }
   debug_assert(end < 4096);
   remainder_block_start = (char *)qh + qh->length;

   if (start == end) {
      rc = 0;
      goto out;
   }

   for (i = start; i < end; i++) {
      if (quick_get_remainder(cfg, &params, remainder_block_start, i) == remainder) {
         rc = 1;
         goto out;
      }
   }

   rc = 0;
out:
   quick_unget_header(cc, filter_node);
   return rc;
}

/*
 *-----------------------------------------------------------------------------
 *
 * quick_async_set_state --
 *
 *      Set the state of the async filter lookup state machine.
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
quick_async_set_state(quick_async_ctxt *ctxt,
                      quick_async_state new_state)
{
   ctxt->prev_state = ctxt->state;
   ctxt->state = new_state;
}


/*
 *-----------------------------------------------------------------------------
 *
 * quick_filter_async_callback --
 *
 *      Callback that's called when the async cache get loads a page into
 *      the cache. This funciton moves the async filter lookup state machine's
 *      state ahead, and calls the upper layer callback that'll re-enqueue
 *      the filter lookup for dispatch.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
quick_filter_async_callback(cache_async_ctxt *cache_ctxt)
{
   quick_async_ctxt *ctxt = cache_ctxt->cbdata;

   platform_assert(SUCCESS(cache_ctxt->status));
   platform_assert(cache_ctxt->page);
//   platform_log("%s:%d tid %2lu: ctxt %p is callback with page %p\n",
//                __FILE__, __LINE__, platform_get_tid(), ctxt,
//                cache_ctxt->page);
   ctxt->was_async = TRUE;
   // Move state machine ahead and requeue for dispatch
   if (ctxt->state == quick_async_state_get_index) {
      quick_async_set_state(ctxt, quick_async_state_got_index);
   } else {
      debug_assert(ctxt->state == quick_async_state_get_filter);
      quick_async_set_state(ctxt, quick_async_state_got_filter);
   }
   ctxt->cb(ctxt);
}


/*
 *-----------------------------------------------------------------------------
 *
 * quick_filter_lookup_async --
 *
 *      Async filter lookup api. Returns if lookup found a key in *found.
 *      The ctxt should've been initialized using quick_filter_ctxt_init().
 *      The return value can be either of:
 *      async_locked: A page needed by lookup is locked. User should retry
 *      request.
 *      async_no_reqs: A page needed by lookup is not in cache and the IO
 *      subsytem is out of requests. User should throttle.
 *      async_io_started: Async IO was started to read a page needed by the
 *      lookup into the cache. When the read is done, caller will be notified
 *      using ctxt->cb, that won't run on the thread context. It can be used
 *      to requeue the async lookup request for dispatch in thread context.
 *      When it's requeued, it must use the same function params except found.
 *      success: Results are in *found
 *
 * Results:
 *      Async result.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

cache_async_result
quick_filter_lookup_async(cache                *cc,               // IN
                          quick_filter_config  *cfg,              // IN
                          uint64                filter_addr,      // IN
                          uint64                num_fingerprints, // IN
                          char                 *key,              // IN
                          int                  *found,            // OUT
                          quick_async_ctxt     *ctxt)             // IN
{
   cache_async_result res = 0;
   bool done = FALSE;

   do {
      switch (ctxt->state) {
      case quick_async_state_start:
      {
         // Calculate filter parameters for the key
         platform_assert(filter_addr != 0);
         quick_compute_params_for_key(cfg, num_fingerprints, key,
                                      &ctxt->params, &ctxt->bucket,
                                      &ctxt->index, &ctxt->remainder);
         ctxt->page_addr = filter_addr +
            cfg->page_size * (ctxt->index / cfg->addrs_per_page);
         quick_async_set_state(ctxt, quick_async_state_get_index);
         // fallthrough;
      }
      case quick_async_state_get_index:
      case quick_async_state_get_filter:
      {
         // Get the index or filter page.
         cache_async_ctxt *cache_ctxt = ctxt->cache_ctxt;

         cache_ctxt_init(cc, quick_filter_async_callback, ctxt, cache_ctxt);
         res = cache_get_async(cc, ctxt->page_addr, PAGE_TYPE_FILTER,
                               cache_ctxt);
         switch (res) {
         case async_locked:
         case async_no_reqs:
//            platform_log("%s:%d tid %2lu: ctxt %p is retry\n",
//                         __FILE__, __LINE__, platform_get_tid(), ctxt);
            /*
             * Ctxt remains at same state. The invocation is done, but
             * the request isn't; and caller will re-invoke me.
             */
            done = TRUE;
            break;
         case async_io_started:
//            platform_log("%s:%d tid %2lu: ctxt %p is io_started\n",
//                         __FILE__, __LINE__, platform_get_tid(), ctxt);
            // Invocation is done; request isn't. Callback will move state.
            done = TRUE;
            break;
         case async_success:
            ctxt->was_async = FALSE;
            if (ctxt->state == quick_async_state_get_index) {
               quick_async_set_state(ctxt, quick_async_state_got_index);
            } else {
               debug_assert(ctxt->state == quick_async_state_get_filter);
               quick_async_set_state(ctxt, quick_async_state_got_filter);
            }
            break;
         default:
            platform_assert(0);
         }
         break;
      }
      case quick_async_state_got_index:
      {
         // Got the index; find address of filter page
         cache_async_ctxt *cache_ctxt = ctxt->cache_ctxt;

         if (ctxt->was_async) {
            cache_async_done(cc, PAGE_TYPE_FILTER, cache_ctxt);
         }
         ctxt->header_addr = ((uint64 *)cache_ctxt->page->data)[
            ctxt->index % cfg->addrs_per_page
         ];
         ctxt->page_addr =
            ctxt->header_addr - (ctxt->header_addr % cfg->page_size);
         cache_unget(cc, cache_ctxt->page);
         quick_async_set_state(ctxt, quick_async_state_get_filter);
         break;
      }
      case quick_async_state_got_filter:
      {
         // Got the filter; find bucket and search for remainder
         cache_async_ctxt *cache_ctxt = ctxt->cache_ctxt;

         if (ctxt->was_async) {
            cache_async_done(cc, PAGE_TYPE_FILTER, cache_ctxt);
         }
         quick_header *qh = (quick_header *)
            (cache_ctxt->page->data + (ctxt->header_addr % cfg->page_size));
         uint64 start, end;
         quick_get_bucket_bounds(qh->header, qh->length,
                                 ctxt->bucket % cfg->index_size, &start, &end);
         platform_assert(start < 4096 && end < 4096 && end >= start);
         char *remainder_block_start = (char *)qh + qh->length;

         *found = 0;
         for (uint64 i = start; i < end; i++) {
            if (quick_get_remainder(cfg, &ctxt->params, remainder_block_start,
                                    i) == ctxt->remainder) {
               *found = 1;
               break;
            }
         }
         cache_unget(cc, cache_ctxt->page);
         res = async_success;
         done = TRUE;
         break;
      }
      default:
         platform_assert(0);
      }
   } while (!done);

   return res;
}

void
quick_filter_deinit(cache               *cc,
                    quick_filter_config *cfg,
                    uint64               filter_addr)
{
   page_handle *index_node = cache_get(cc, filter_addr, TRUE, PAGE_TYPE_FILTER);
   // the page of extent addrs is the first page of the extent
   // of filter pages; we get there by looking up the first filter
   // pages and calculating the first page in that extent
   //uint64 debug_addr = index_node->disk_addr;
   uint64 extent_page_addr = *(uint64 *)(index_node->data);
   extent_page_addr -= extent_page_addr % cfg->extent_size;
   page_handle *extent_node = cache_get(cc, extent_page_addr, TRUE,
         PAGE_TYPE_FILTER);
   //uint64 debug_addr2 = extent_node->disk_addr;
   cache_unget(cc, index_node);
   uint64 *filter_extent_addr = (uint64 *)extent_node->data;
   uint64 num_filter_extent_addrs = *filter_extent_addr;
   //platform_log("filter deinit: %12lu %12lu %12lu\n", filter_addr,
   //      num_filter_extent_addrs, extent_page_addr);
   //platform_log("             : %12lu                %lu\n", debug_addr, debug_addr2);
   filter_extent_addr++;
   // start at one because we are using the first page and dealloc it
   // separately afterwards
   for (uint64 i = 1; i < num_filter_extent_addrs; i++)
      cache_dealloc(cc, filter_extent_addr[i], PAGE_TYPE_FILTER);
   cache_unget(cc, extent_node);
   cache_dealloc(cc, extent_page_addr, PAGE_TYPE_FILTER);
   cache_dealloc(cc, filter_addr, PAGE_TYPE_FILTER);
}

void
quick_filter_verify(cache               *cc,
                    quick_filter_config *cfg,
                    uint64               filter_addr,
                    uint64               num_tuples,
                    iterator            *itor)
{
   bool at_end;
   iterator_at_end(itor, &at_end);
   uint64 index = 0;
   while (!at_end) {
      char *key;
      char *data;
      data_type type;
      iterator_get_curr(itor, &key, &data, &type);
      bool found = quick_filter_lookup(cc, cfg, filter_addr, num_tuples, key);
      platform_assert(found);
      iterator_advance(itor);
      index++;
      iterator_at_end(itor, &at_end);
   }
}

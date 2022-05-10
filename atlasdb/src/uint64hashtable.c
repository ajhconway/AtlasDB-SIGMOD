#include "platform.h"
#include "random.h"

#include "poison.h"

#define HASH_BITS (64)
#define HASH_MASK (HASH_BITS == 64 ? 0xffffffffffffffff : ((1ULL << HASH_BITS) - 1))

#define LOCK_GRANULARITY (2)

typedef struct slot {
  uint64 hash;
  uint64 value;
} slot;

typedef struct uint64hashtable {
  uint64 lognslots;
  uint64 nslots;
  uint64 xnslots;
  uint64 empty_hash;
  uint64 ignored_hash;
  int8 *locks;
  slot *slots;
} uint64hashtable;

static
void __attribute__((__unused__)) dump(uint64hashtable *ht)  {
  uint64 i;

  for (i = 0; i < (ht->xnslots + LOCK_GRANULARITY - 1) / LOCK_GRANULARITY; i++)
    platform_log("%-*d ", LOCK_GRANULARITY * ((HASH_BITS + 3) / 4 + 1) - 1, ht->locks[i]);
  platform_log("\n");
  for (i = 0; i < ht->xnslots; i++) {
    if (ht->slots[i].hash == ht->empty_hash)
      platform_log("%*s ", (HASH_BITS + 3) / 4, " ");
    else if (ht->slots[i].hash == ht->ignored_hash)
      platform_log("%*s ", (HASH_BITS + 3) / 4, "i");
    else
      platform_log("%0*lx ", (HASH_BITS + 3) / 4, ht->slots[i].hash);
  }
  platform_log("\n");
  for (i = 0; i < ht->xnslots; i++)
    platform_log("%0*lx ", (HASH_BITS + 3) / 4, ht->slots[i].value);
  platform_log("\n");
}

/*
 *  Thomas Wang's integer hash functions. See
 *  <https://gist.github.com/lh3/59882d6b96166dfc3d8d> for a snapshot.
 *
 *   For any 1<k<=64, let mask=(1<<k)-1. hash_64() is a bijection on [0,1<<k),
 *   which means
 *     hash_64(x, mask)==hash_64(y, mask) if and only if x==y. hash_64i() is
 *     the inversion of
 *       hash_64(): hash_64i(hash_64(x, mask), mask) == hash_64(hash_64i(x,
 *       mask), mask) == x.
 */

static inline uint64_t hash_64(uint64_t key, uint64_t mask)
{
	key = (~key + (key << 21)) & mask; // key = (key << 21) - key - 1;
	key = key ^ key >> 24;
	key = ((key + (key << 3)) + (key << 8)) & mask; // key * 265
	key = key ^ key >> 14;
	key = ((key + (key << 2)) + (key << 4)) & mask; // key * 21
	key = key ^ key >> 28;
	key = (key + (key << 31)) & mask;
	return key;
}

static inline uint64_t hash(uint64_t key) {
  return hash_64(key, HASH_MASK);
}

int uint64hashtable_init(uint64hashtable *ht, uint64 lognslots, uint64 invalid_key1, uint64 invalid_key2) {
  platform_assert(lognslots <= HASH_BITS);

  uint64 nslots = 1ULL << lognslots;
  uint64 xnslots = 2 * nslots;
  uint64 nlocks = (xnslots + LOCK_GRANULARITY - 1) / LOCK_GRANULARITY;
  int8 *locks = TYPED_ARRAY_ZALLOC(platform_get_heap_id(), locks, nlocks);
  if (!locks)
    return -1;

  slot *slots = TYPED_ARRAY_MALLOC(platform_get_heap_id(), slots, xnslots);
  if (!slots) {
    platform_free(platform_get_heap_id(), locks);
    return -1;
  }

  ht->lognslots = lognslots;
  ht->nslots = nslots;
  ht->xnslots = xnslots;
  ht->empty_hash = hash(invalid_key1);
  ht->ignored_hash = hash(invalid_key2);
  ht->locks = locks;
  ht->slots = slots;

  uint64 i;
  for (i = 0; i < xnslots; i++)
    slots[i].hash = ht->empty_hash;

  return 0;
}

static uint64 home_slot_for_hash(uint64hashtable *ht, uint64 hash) {
  return hash >> (HASH_BITS - ht->lognslots);
}



int uint64hashtable_lookup(uint64hashtable *ht, uint64 key, uint64 *value) {
  uint64 key_hash = hash(key);

  if (key_hash == ht->empty_hash || key_hash == ht->ignored_hash)
    return -2;

  uint64 home_slot_index = home_slot_for_hash(ht, key_hash);

  uint64 i = home_slot_index;
  while (i < ht->xnslots && ht->slots[i].hash != ht->empty_hash) {
    if (ht->slots[i].hash == key_hash) {
      *value = ht->slots[i].value;
      return 0;
    }
    i = i + 1;
  }

  if (ht->xnslots <= i)
    return -3;

  return -1;
}

static void lock_slot(uint64hashtable *ht, uint64 i) {
  platform_assert(i < ht->xnslots);
  platform_assert(ht->locks[i / LOCK_GRANULARITY] == 0);
  ht->locks[i / LOCK_GRANULARITY] = 1;
}

static void unlock_slot(uint64hashtable *ht, uint64 i) {
  platform_assert(i < ht->xnslots);
  platform_assert(ht->locks[i / LOCK_GRANULARITY] == 1);
  ht->locks[i / LOCK_GRANULARITY] = 0;
}

/* Inserts key-value only if key is not already present. Returns
   non-zero otherwise. */
int uint64hashtable_insert(uint64hashtable *ht, uint64 key, uint64 value) {
  uint64 key_hash = hash(key);

  if (key_hash == ht->empty_hash || key_hash == ht->ignored_hash)
    return -2;

  uint64 home_slot_index = home_slot_for_hash(ht, key_hash);

  uint64 i = home_slot_index;
  lock_slot(ht, i);

  while (i < ht->xnslots && ht->slots[i].hash != ht->empty_hash) {
    if (ht->slots[i].hash == key_hash) {
      unlock_slot(ht, i);
      return -1;
    }
    uint64 nexti = i + 1;
    if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY) {
      lock_slot(ht, nexti);
      if (ht->slots[i].hash < key_hash)
        unlock_slot(ht, i);
    }
    i = nexti;
  }

  if (ht->xnslots <= i)
    return -3;

  uint64 previ = i-1;
  while (i != home_slot_index && key_hash < ht->slots[previ].hash) {
    ht->slots[i] = ht->slots[previ];
    if (i / LOCK_GRANULARITY != previ / LOCK_GRANULARITY)
      unlock_slot(ht, i);
    i = previ;
    previ = i-1;
  }

  ht->slots[i].hash = ht->ignored_hash;
  ht->slots[i].value = value;
  ht->slots[i].hash = key_hash;
  unlock_slot(ht, i);

  return 0;
}

int uint64hashtable_delete(uint64hashtable *ht, uint64 key) {
  uint64 key_hash = hash(key);

  if (key_hash == ht->empty_hash || key_hash == ht->ignored_hash)
    return -2;

  uint64 home_slot_index = home_slot_for_hash(ht, key_hash);

  uint64 i = home_slot_index;
  lock_slot(ht, i);

  while (i < ht->xnslots && ht->slots[i].hash != ht->empty_hash && ht->slots[i].hash < key_hash) {
    uint64 nexti = i + 1;
    if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY) {
      lock_slot(ht, nexti);
      unlock_slot(ht, i);
    }
    i = nexti;
  }

  if (ht->slots[i].hash != key_hash) {
    unlock_slot(ht, i);
    return -1;
  }

  ht->slots[i].hash = ht->ignored_hash;

  uint64 nexti = i + 1;
  if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY)
    lock_slot(ht, nexti);
  while (i < ht->xnslots &&
         ht->slots[nexti].hash != ht->empty_hash &&
         home_slot_for_hash(ht, ht->slots[nexti].hash) != nexti) {
    ht->slots[i].value = ht->slots[nexti].value;
    ht->slots[i].hash = ht->slots[nexti].hash;

    if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY) {
      unlock_slot(ht, i);
    }
    i = nexti;
    nexti = i + 1;
    if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY)
      lock_slot(ht, nexti);
  }

  if (ht->xnslots <= i)
    return -3;

  ht->slots[i].hash = ht->empty_hash;
  unlock_slot(ht, i);
  if (i / LOCK_GRANULARITY != nexti / LOCK_GRANULARITY) {
    unlock_slot(ht, nexti);
  }

  return 0;
}

#define TEST_SIZE (25)
static uint64 reference[1<<TEST_SIZE];

static int __attribute__((__unused__)) test_main(int argc, char **argv) {
  uint64hashtable ht;
  int res;
  random_state rs;

  random_init(&rs, platform_get_real_time(), 0);

  //memset(reference, 0, sizeof(reference));

  res = uint64hashtable_init(&ht, 2+TEST_SIZE, (uint64)(-2), (uint64)(-1));
  platform_assert(res == 0);

  uint64 nextval = 1;
  while (nextval < (1ULL << 28)) {
    uint64 key = random_next_uint64(&rs) % (1ULL << TEST_SIZE);
    //uint64 val = 0;

    //dump(&ht);
    //platform_log("key %lx, val %lx\n", key, reference[key]);

    //res = uint64hashtable_lookup(&ht, key, &val);
    res = reference[key] == 0 ? -1 : 0;
    platform_assert(res == -1 || res == 0);
    if (res == -1) {
      platform_assert(reference[key] == 0);
    } else {
      //assert(reference[key] == val);
    }

    if (res == -1) {
      reference[key] = nextval++;
      //uint64hashtable_insert(&ht, key, reference[key]);
    } else {
      //uint64hashtable_delete(&ht, key);
      reference[key] = 0;
    }
  }

  return 0;
}

#if 0
int test_main(int argc, char **argv) {
  return test_main(argc, argv);
}
#endif

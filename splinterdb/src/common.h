/*************************************************************************
 * Copyright 2020 VMware, Inc. All rights reserved -- VMware Confidential
 *************************************************************************/


#ifndef _COMMON_H
#define _COMMON_H

typedef uint64 splinter_id;

#define MAX_SPLINTER_TABLES_PER_DISK (30)

#define SPLINTER_INVALID_ID (0)

#define SPLINTER_COMMON_CSUM_SEED (42)

#define SPLINTER_NUM_MEMTABLES (4)
#define SPLINTER_INVALID_MEMTABLE_GENERATION (UINT64_MAX)

#endif

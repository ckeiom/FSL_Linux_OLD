#ifndef _LIBLOCKDEP_LINUX_TYPES_H_
#define _LIBLOCKDEP_LINUX_TYPES_H_

#include <stdbool.h>
#include <stddef.h>

#define __SANE_USERSPACE_TYPES__	/* For PPC64, to get LL64 types */
#include <asm/types.h>

struct page;
struct kmem_cache;

typedef unsigned gfp_t;

typedef __u64 u64;
typedef __s64 s64;

typedef __u32 u32;
typedef __s32 s32;

typedef __u16 u16;
typedef __s16 s16;

typedef __u8  u8;
typedef __s8  s8;

#ifdef __CHECKER__
#define __bitwise__ __attribute__((bitwise))
#else
#define __bitwise__
#endif
#ifdef __CHECK_ENDIAN__
#define __bitwise __bitwise__
#else
#define __bitwise
#endif


typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#endif
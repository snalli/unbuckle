

#include <db/hashtable.h>
#include <db/spooky/spooky_hash.h>
#include <entry.h>
#include <uberrors.h>

#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define HASHTABLE_SIZE_BITS 24
#define SPOOKY_SEED 0xDEADBEEFFEEDCAFELL

/* Definition of opaque type bucket_lock_ot  (see entry.h)
   (we might not always be using spinlocks).               */
struct bucket_lock
{
	spinlock_t lock;
};

DECLARE_HASHTABLE(hashtable, HASHTABLE_SIZE_BITS);
static bucket_lock_ot slocks[(1 << HASHTABLE_SIZE_BITS)];

/* forward declarations */
static int ub_hashtbl_lock_bucket(bucket_lock_ot *lock);
static bucket_lock_ot *get_bucket_lock_by_key(u64 key_hash);

/* use spooky hash to get variable length char* arrays used as keys down into a
   constant bit length which is compatible with the kernel hash table (the kernel
   doesn't have a variable length hashing function as far as I can see */
static inline uint64 get_spooky64_hash(char* key, int len_key)
{
	return spooky_Hash64(key, len_key, SPOOKY_SEED);
}

int ub_hashtbl_init(void)
{
	hash_init(hashtable);
	return 0;
}

static void hashtbl_empty_all(void)
{
	int bkt;
	struct ub_entry* e;

	if (hash_empty(hashtable))
		return;
	
	// Need to walk the hash table to unset every struct hlist_node structure
	hash_for_each_rcu(hashtable, bkt, e, hlist)
	{
		kfree_skb(e->skb);
		hash_del_rcu(&e->hlist);
	}

	return;
}

void ub_hashtbl_exit(void)
{
	hashtbl_empty_all();
	return;
}

int ub_hashtbl_add(struct ub_entry* e)
{
	struct hlist_node* node = &e->hlist;
	char* key = ub_entry_loc_key(e);
	size_t len_key = e->len_key;

	// the kernel hash table doesn't give any feedback as to success or failure

	// Hash the key down to a form which the kernel hash table can use
	uint64 key_hash = get_spooky64_hash(key, len_key);
	hash_add_rcu(hashtable, node, key_hash);	
	return 0;
}

struct ub_entry* ub_hashtbl_find(char* key, size_t len_key)
{
	struct ub_entry* e;
	uint64 key_hash;

	// Hash the key down to a form which the kernel hash table can use
	key_hash = get_spooky64_hash(key, len_key);
	
	hash_for_each_possible_rcu(hashtable, e, hlist, key_hash)
	{
		if (e->len_key != len_key)
			continue;

		if (!strncmp(key, ub_entry_loc_key(e), len_key))
		{
			return e;
			break;
		}
	}
	
	// key not found
	return NULL;
}

/* The caller must already hold the bucket lock if this is invoked
   (which should be so because this is only currently invoked if there is a key clash
   in the hash table -- it doesn't get invoked explicitly because we don't expose a 
   deletion operation.) */
void ub_hashtbl_del(struct ub_entry* e)
{
	hash_del_rcu(&e->hlist);
}

/******************************************************************
 * Methods to manipulate bucket locks in the hash table go here.  *
 ******************************************************************/

static bucket_lock_ot *get_bucket_lock_by_key(u64 key_hash)
{
	return &slocks[hash_min(key_hash, HASH_BITS(hashtable))];
}
	
static int ub_hashtbl_lock_bucket(bucket_lock_ot *lock)
{
	spin_lock(&lock->lock);
	return 1;
}
void ub_hashtbl_unlock_bucket(bucket_lock_ot *lock)
{
	spin_unlock(&lock->lock);
	return;
}

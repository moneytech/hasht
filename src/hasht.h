/*
Copyright 2019 Turki Alsaleem

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/*
needed functions that should be defined before including this: 
    int    hasht_key_eq_cmp(hasht_key_type *key_1, hasht_key_type *key_2) (returns 0 if equal)
    size_t hasht_hash(hasht_key_type *key)

    #if HASHT_DATA_ARG is defined then the prototypes will be
    int    hasht_key_eq_cmp(void *udata, hasht_key_type *key_1, hasht_key_type *key_2) (returns 0 if equal)
    size_t hasht_hash(void *udata, hasht_key_type *key)

    udata is in struct hasht, you're supposed to set it directly when you initialize the hashtable
needed typedefs: 
     typedef <type> hasht_key_type; 
     typedef <type> hasht_value_type; 

    as an example: key_type can be a string (char *)
                   value_type can be a struct foo; for example
    in this example, the function hasht_key_cmp would recieve: char **, char **
                     the function hasht would recieve struct foo *
                     the same is true for the api's functions, insert() expects (key: char **, value: struct foo *)
                                                               remove() would expect (key: char **)
*/


//the following is an anti-include-guard

#ifdef HASHT_H
#error "the header can only be safely included once"
#endif // #ifdef HASHT_H
#define HASHT_H

#include <stdlib.h> //malloc, free
#include <stdbool.h> 
#include <string.h> //memset, memcpy, ...
#include "div_32_funcs.h" //divison functions


static int hasht_get_adiv_power_idx(size_t at_least) {
    //starts at 2
    for (int i=2; i < (int)adiv_n_values; i++) {
        if (adiv_values[i] >= at_least) 
            return i;
    }
    return -1; //error, must be handled
}

//#define HASHT_DBG

#define HASHT_MIN_TABLESIZE 4

#ifdef HASHT_DBG
    #include <assert.h>
    #define HASHT_DBG_CODE(...) do { __VA_ARGS__ } while(0)
    #define HASHT_ASSERT(cond, msg) assert(cond)
#else
    #define HASHT_DBG_CODE(...) 
    #define HASHT_ASSERT(cond, msg) 
#endif

//notice how logic is inverted? so we can memset with 0
#define HASHT_VLT_IS_NOT_EMPTY      (1U << 1)
#define HASHT_VLT_IS_DELETED        (1U << 2)
#define HASHT_VLT_IS_CORRUPT        (1U << 3)
//long is used for all lengths / sizes


struct hasht_pair_type {
    //bits:
    //[0...8]  flags
    //[8..32]  partial hash
    unsigned int pair_data; //this is two parts: the flags, and the partial hash
    hasht_key_type   key;
    hasht_value_type value;
};

//pair type functions
static unsigned char hasht_pr_flags(struct hasht_pair_type *prt) {
    return prt->pair_data & 0xFF;
}
static unsigned int hasht_pr_get_partial_hash(struct hasht_pair_type *prt) {
    const unsigned int upper_24bits = 0xFFFFFF00;
    return prt->pair_data & upper_24bits;
}
//partial hashes have the lower 8 bits equal to zero, (there we store flags)
//this is assuming unsigned int is at least 32 bits, 
//the alternative would be uint32_t stuff
static unsigned int hasht_hash_to_partial_hash(size_t full_hash) {
    const unsigned int lower_24bits = 0x00FFFFFF;
    return (full_hash & lower_24bits) << 8;
}
static unsigned int hasht_pr_combine_flags_and_partial_hash(unsigned char flags, unsigned int partial_hash) {
    HASHT_ASSERT((partial_hash & 0xFF) == 0, "invalid partial hash");
    return partial_hash | flags;
}
static void hasht_pr_set_flags(struct hasht_pair_type *prt, unsigned char flags) {
    prt->pair_data = hasht_pr_combine_flags_and_partial_hash(flags, hasht_pr_get_partial_hash(prt));
}


//[is_deleted] [is_not_empty]
//     0            0          empty
//     0            1          occupied
//     1            0          invalid state
//     1            1          deleted

static bool hasht_pr_is_empty(struct hasht_pair_type *prt) {
    return (! (hasht_pr_flags(prt) & HASHT_VLT_IS_NOT_EMPTY)); //false mean occupied or deleted
}
static bool hasht_pr_is_corrupt(struct hasht_pair_type *prt) {
    const unsigned char deleted_and_empty_mask = (HASHT_VLT_IS_DELETED | HASHT_VLT_IS_NOT_EMPTY); 
    const unsigned char deleted_and_empty      = (HASHT_VLT_IS_DELETED | 0); //invalid state
    return   (hasht_pr_flags(prt) & HASHT_VLT_IS_CORRUPT) || 
             ((hasht_pr_flags(prt) & deleted_and_empty_mask) == deleted_and_empty);
}
static bool hasht_pr_is_deleted(struct hasht_pair_type *prt) {
    return   (hasht_pr_flags(prt) & HASHT_VLT_IS_DELETED); //false means occupied or empty
}
static bool hasht_pr_is_occupied(struct hasht_pair_type *prt) {
    //occupied here means an active bucket that contains a value
    HASHT_ASSERT(!hasht_pr_is_corrupt(prt), "corrupt element found");
    return !hasht_pr_is_empty(prt) && !hasht_pr_is_deleted(prt); 
}

typedef void * (*hasht_malloc_fptr)(size_t sz, void *userdata);
typedef void * (*hasht_realloc_fptr)(void *ptr, size_t sz, void *userdata);
typedef void (*hasht_free_fptr)(void *ptr, void *userdata);

struct hasht_alloc_funcs {
    hasht_malloc_fptr alloc;
    hasht_realloc_fptr realloc;
    hasht_free_fptr free;
};
static void *hasht_def_malloc(size_t sz, void *unused_userdata_) {
    (void) unused_userdata_;
    return malloc(sz); 
}
static void *hasht_def_realloc(void *ptr, size_t sz, void *unused_userdata_) {
    (void) unused_userdata_;
    return realloc(ptr, sz); 
}
static void  hasht_def_free(void *ptr, void *unused_userdata_) {
    (void) unused_userdata_;
    free(ptr); 
}

enum HASHT_ERR {
    HASHT_OK,
    HASHT_ALLOC_ERR,
    HASHT_INVALID_REQ_SZ,
    HASHT_FAILED_AT_RESIZE,

    HASHT_NOT_FOUND = -1, 
    HASHT_DUPLICATE_KEY = -2,
    //the table refused to change the size, becuase it thinks it's smart and there's no need, this can be forced though (WIP)
    HASHT_RESIZE_REFUSE = -3, 
    HASHT_ITER_STOP = -4,
    HASHT_ITER_FIRST = -5,

    HASHT_INVALID_TABLE_STATE = -6, //non recoverable, the only safe operation to do is to call deinit
};

//Careful with changes!, the struct is migrated to a new one in hasht_resize__
struct hasht {
    struct hasht_pair_type *tab;
    adiv_fptr div_func; //a pointer to a function that does fast division

    long nelements; //number of active buckets (ones that are not empty, and not deleted)
    long ndeleted;
    long nbuckets;
    long nbuckets_po2; //power of two
    long grow_at_gt_n;  //saved result of computation
    long shrink_at_lt_n; 
    long grow_at_percentage; // (divide by 100, for example 0.50 is 50)
    long shrink_at_percentage; 
    struct hasht_alloc_funcs memfuncs;
    void *userdata;

};


//shrink at, grow at are percentages [0, 99] inclusive, they must fulfil (grow_at / shrink_at) > 2.0
//the function can fail
static int hasht_init_parameters(struct hasht *ht, long shrink_at, long grow_at) {
    bool stupid_value = (shrink_at > 99 || shrink_at < 0 || grow_at > 99 || grow_at < 0);
    if (stupid_value || (shrink_at*2 >= grow_at)) {
        //this fails when, (grow_at / shrink_at) <= 2.0
        //for example, suppose you have shrink_at = 20, grow_at = 35, nelements = 19, nbuckets = 100
        //when we shrink, we shrink by dividing by two, so then we have:
        //nelements = 19, nbuckets = 50
        //now 19/50 is 38%, which means we need to grow again!, therfore grow_at must be greater than shrink_at * 2, preferably way greater
        //a sane value is like 2 to 6, or 2 to 8
        //good values: (20, 60), (15, 45), (15, 50), (15, 60)
        //bad values: (20, 30), (30, 50) etc..
        return HASHT_INVALID_REQ_SZ;
    }
    ht->grow_at_percentage   = grow_at;
    ht->shrink_at_percentage = shrink_at;

    return HASHT_OK;
}

//see notes above in hasht_init_parameters(), they apply to this function
static int hasht_set_parameters(struct hasht *ht, long shrink_at, long grow_at) {
    int rv = hasht_init_parameters(ht, shrink_at, grow_at);
    if (rv != HASHT_OK)
        return rv;
    //this can potentially overflow, maybe we should cast to size_t
    ht->grow_at_gt_n = (ht->nbuckets * ht->grow_at_percentage) / 100;
    ht->shrink_at_lt_n = (ht->nbuckets * ht->shrink_at_percentage) / 100;
    return rv;
}

static long hasht_calc_nelements_to_nbuckets(long needed_nelements, long shrink_at_percentage, long grow_at_percentage) {
    //let ratio = needed_nelements / x 
    //we want an x that fullfills: 
    //ratio > shrink_percent AND ratio < grow_percent
    //pick any ratio between the two: r1 = (shrink_percent + grow_percent) / 2
    //x would be = needed_elements / r1
    //
    //since we store percentages as ints implictly a fraction of 100, r1 = some_integer / 100
    //x = needed_nelements * (100 / some_integer)
    //x = needed_nelements * 100 / some_integer
    long r1i = (shrink_at_percentage + grow_at_percentage) / 2;
    r1i = r1i <= 0 ? 1 : r1i;
    long needed_nbuckets_opt_big = (needed_nelements * 100) / r1i;
    r1i = grow_at_percentage - (grow_at_percentage - shrink_at_percentage) / 3;
    long needed_nbuckets_opt_small = needed_nbuckets_opt_big;
    if (r1i > 0) 
        needed_nbuckets_opt_small = (needed_nelements * 100) / r1i;

    long needed_nbuckets = needed_nbuckets_opt_big;

    if (needed_nbuckets > HASHT_MIN_TABLESIZE) {
#ifdef HASHT_DBG
        long ratio_test       = ((needed_nelements * 100) / needed_nbuckets);
        HASHT_ASSERT( ratio_test >= shrink_at_percentage, "ratio calculation failed");
        HASHT_ASSERT( ratio_test <= grow_at_percentage, "ratio calculation failed");
#endif
        long ratio_test_small = ((needed_nelements * 100) / needed_nbuckets_opt_small);
        if (ratio_test_small > shrink_at_percentage)
            needed_nbuckets = needed_nbuckets_opt_small;
    }
    else {
        //clamp to the minimum
        needed_nbuckets = HASHT_MIN_TABLESIZE;
    }
    HASHT_ASSERT(needed_nbuckets > needed_nelements, "ratio calculation failed");
    return needed_nbuckets;
}

//this is not very trivial, multiple fields are tied to each other and they need to be consistent
//if this fails it doesnt change the field
static int hasht_change_sz_field(struct hasht *ht, long nbuckets, bool force) {
    (void) force;
    //ignore values that are too small
    nbuckets = nbuckets < HASHT_MIN_TABLESIZE ? HASHT_MIN_TABLESIZE : nbuckets;
    long nbuckets_po2 = hasht_get_adiv_power_idx(nbuckets);
    if (nbuckets_po2 < 0)
        return HASHT_INVALID_REQ_SZ; //too big
    HASHT_ASSERT(nbuckets_po2 >= 2 && nbuckets_po2 < adiv_n_values, "adiv failed");
    long nbuckets_prime = adiv_values[nbuckets_po2];
    ht->nbuckets     = nbuckets_prime;
    ht->nbuckets_po2 = nbuckets_po2;
    ht->div_func = adiv_funcs[ht->nbuckets_po2];
    HASHT_ASSERT(ht->div_func, "invalid adiv fptr");

#ifdef HASHT_DBG
    long rnd = rand();
    HASHT_ASSERT(ht->div_func(rnd) == rnd % ht->nbuckets, "adiv failed");
#endif// HASHT_DBG
    
    HASHT_ASSERT(ht->shrink_at_percentage > 0 && ht->grow_at_percentage > ht->shrink_at_percentage, "invalid growth parameters");
    //update cached result of division
    hasht_set_parameters(ht, ht->shrink_at_percentage, ht->grow_at_percentage);
    return HASHT_OK;
}
static void hasht_zero_sz_field(struct hasht *ht) {
    ht->nbuckets = 0;
    ht->nbuckets_po2 = 0;
    ht->div_func = NULL;
}

//tries to find memory corruption
//flags are flags to consider a failure
//for the three query* ints, 0 means i dont care, 1 expect it to be, -1 means expect it to be not
//for example hasht_dbg_check(ht, 0, ht->nbuckets, 0, 0, -1) -> fail if any are corrupt
static bool hasht_dbg_check(struct hasht *ht, long beg_idx, long end_idx, int query_empty, int query_deleted, int query_corrupt) {
    long len = end_idx - beg_idx;
    for (int i=0; i<len; i++) {
        struct hasht_pair_type *pair = ht->tab + i + beg_idx;
        int expect[3] = {
            query_empty,
            query_deleted,
            query_corrupt, 
        };
        int found[3] = { 
            hasht_pr_is_empty(pair),
            hasht_pr_is_deleted(pair),
            hasht_pr_is_corrupt(pair),
        };
        for (int i=0; i<3; i++) {
            if (((expect[i] > 0) && !found[i]) || ((expect[i] < 0) && found[i]))
                return false;
        }
    }
    return true;
}

static bool hasht_dbg_sanity_01(struct hasht *ht) {
    return ht->tab &&
           ht->nbuckets &&
           (ht->nbuckets_po2 == hasht_get_adiv_power_idx(ht->nbuckets)) &&
           (ht->shrink_at_lt_n < ht->grow_at_gt_n) &&
           ht->div_func;
}
static bool hasht_dbg_sanity_heavy(struct hasht *ht) {
    return hasht_dbg_sanity_01(ht) && hasht_dbg_check(ht, 0, ht->nbuckets, 0, 0, -1);
}

static void hasht_memset(struct hasht *ht, long begin_inc, long end_exc) {
    HASHT_ASSERT(hasht_dbg_sanity_01(ht), "hasht corrupt or not initialized");
    //the flags are designed so that memsetting with 0 means: empty, not deleted, not corrupt
    memset(ht->tab + begin_inc, 0, sizeof(struct hasht_pair_type) * (end_exc - begin_inc));
    HASHT_ASSERT(hasht_dbg_check(ht, begin_inc, end_exc, 1, -1, -1), "");
}

static int hasht_init_ex(struct hasht *ht,
                        long initial_nelements, 
                        hasht_malloc_fptr alloc,
                        hasht_realloc_fptr realloc,
                        hasht_free_fptr free,
                        void *userdata,
                        long shrink_at_percentage,
                        long grow_at_percentage)
{
    int rv;
#ifdef HASHT_DBG
    memset(ht, 0x3c, sizeof *ht);
#endif
    const struct hasht_alloc_funcs memfuncs = { alloc, realloc, free, };
    ht->memfuncs = memfuncs;
    ht->nelements = 0;
    ht->ndeleted = 0;
    ht->nbuckets_po2 = 0;
    ht->userdata = userdata;

    rv = hasht_init_parameters(ht, shrink_at_percentage, grow_at_percentage);
    if (rv != HASHT_OK)
        return rv;

    long initial_nbuckets = hasht_calc_nelements_to_nbuckets(initial_nelements, ht->shrink_at_percentage, ht->grow_at_percentage);
    rv = hasht_change_sz_field(ht, initial_nbuckets, false);
    if (rv != HASHT_OK)
        return rv;

    ht->tab = ht->memfuncs.alloc(sizeof(struct hasht_pair_type) * ht->nbuckets, ht->userdata);
    if (!ht->tab)
        return HASHT_ALLOC_ERR;
    hasht_memset(ht, 0, ht->nbuckets); //mark everything empty
    return HASHT_OK;
}

//returns an empty copy that has the same allocator settings and same parameters
static int hasht_init_copy_settings(struct hasht *ht,
                        long initial_nelements, 
                        const struct hasht *source)
{
    int rv =  hasht_init_ex(ht, //struct hasht *ht,
                        initial_nelements, //long initial_nelements, 
                        source->memfuncs.alloc,//hasht_malloc_fptr alloc,
                        source->memfuncs.realloc,//hasht_realloc_fptr realloc,
                        source->memfuncs.free,// hasht_free_fptr free,
                        source->userdata, //void *userdata,
                        source->shrink_at_percentage, //long shrink_at_percentage,
                        source->grow_at_percentage //long grow_at_percentage)
                        );
    return rv;
}

static int hasht_init_with_udata(struct hasht *ht, long initial_nelements, void *userdata) {
    return hasht_init_ex(ht, //struct hasht *ht,
                        initial_nelements, //long initial_nelements, 
                        hasht_def_malloc,//hasht_malloc_fptr alloc,
                        hasht_def_realloc,//hasht_realloc_fptr realloc,
                        hasht_def_free,// hasht_free_fptr free,
                        userdata, //void *userdata,
                        20, //long shrink_at_percentage,
                        60 //long grow_at_percentage)
                        );
}

static int hasht_init(struct hasht *ht, long initial_nelements) {
    return hasht_init_with_udata(ht, initial_nelements, NULL);
}

static void hasht_deinit(struct hasht *ht) {
    ht->memfuncs.free(ht->tab, ht->userdata);
    ht->tab = NULL;
    hasht_zero_sz_field(ht);
}
static long hasht_integer_mod_buckets(struct hasht *ht, size_t full_hash) {
    long divd_hash = ht->div_func(full_hash); //fast division (% not division) by hardcoded primes
    HASHT_ASSERT(divd_hash < ht->nbuckets, "");
    return divd_hash;
}

//precondition: idx can only be in [-1...nbuckets] (inclusive both ends)
static long hasht_idx_mod_buckets(struct hasht *ht, long idx) {
    HASHT_ASSERT(idx >= -1 && idx <= ht->nbuckets, "");
    if (idx < 0)
        return ht->nbuckets - 1;
    if (idx >= ht->nbuckets)
        return 0;
    return idx;
}

static long hasht_n_unused_buckets(struct hasht *ht) {
    return ht->nbuckets - ht->nelements;
}
static long hasht_n_empty_buckets(struct hasht *ht) {
    return ht->nbuckets - ht->nelements - ht->ndeleted;
}
static long hasht_n_nonempty_buckets(struct hasht *ht) {
    return ht->nelements + ht->ndeleted;
}
//excluding deleted
static long hasht_n_used_buckets(struct hasht *ht) {
    return ht->nelements;
}



//returns 0 if equal
static int hasht_cmp(struct hasht *ht, hasht_key_type *key1, unsigned int partial_hash_1, struct hasht_pair_type *pair) {
    (void) ht;

    //skip full key comparison
    if (hasht_pr_get_partial_hash(pair) != partial_hash_1)
        return 1; 


#ifdef HASHT_DATA_ARG
    HASHT_ASSERT(hasht_key_eq_cmp(ht->userdata, &pair->key, &pair->key) == 0, "hasht_key_eq_cmp() is broken,"
                                                            " testing it on the same key fails to report it's equal to itself");
    return hasht_key_eq_cmp(ht->userdata, key1, &pair->key);
#else
    HASHT_ASSERT(hasht_key_eq_cmp(&pair->key, &pair->key) == 0, "hasht_key_eq_cmp() is broken,"
                                                            " testing it on the same key fails to report it's equal to itself");
    return hasht_key_eq_cmp(           key1, &pair->key);
#endif
}

//on successful match, returns HASHT_OK
//otherwise unless an error occurs it returns NOT_FOUND and out_idx will hold a suggested place to insert 
//if we have no suggested place then out_idx is set to NOT_FOUND too
static inline int hasht_find_pos__(struct hasht *ht, hasht_key_type *key, long *out_idx, size_t *full_hash_out) {
    HASHT_ASSERT(out_idx && full_hash_out, "");

    #ifdef HASHT_DATA_ARG
        size_t full_hash = hasht_hash(ht->userdata, key);
    #else
        size_t full_hash = hasht_hash(key);
    #endif
    unsigned int partial_hash = hasht_hash_to_partial_hash(full_hash);
    *full_hash_out = full_hash;
    long idx = hasht_integer_mod_buckets(ht, full_hash);
    long suggested = HASHT_NOT_FOUND; //suggest where to insert

    if (hasht_n_empty_buckets(ht) < 1) {
        //note that if hasht_n_unused_buckets is anywhere near one it'll be a very a slow search anyways
        HASHT_ASSERT(false, "precondition violated, this leads to an infinite loop");
        return HASHT_INVALID_TABLE_STATE;
    }

    //we can probably use an upper iteration count, in case there is memory corruption, but we just ignore that here, we assume the user is sane
    while (1) {
        struct hasht_pair_type *pair = ht->tab + idx;
        if (hasht_pr_is_occupied(pair)) {
            if (hasht_cmp(ht, key, partial_hash, pair) == 0) {
                *out_idx = idx;
                return HASHT_OK; //found
            }
        }
        else if (hasht_pr_is_deleted(pair)) {
            if (suggested == HASHT_NOT_FOUND)
                suggested = idx; 
        }
        else if (hasht_pr_is_empty(pair)) {
            if (suggested == HASHT_NOT_FOUND)
                suggested = idx;
            *out_idx = suggested;
            return HASHT_NOT_FOUND;
        }
#ifdef HASHT_DBG
        else {
            HASHT_ASSERT(false, "invalid bucket state");
        }
#endif
        idx = hasht_idx_mod_buckets(ht, idx + 1); //this is where we can change linear probing
    }

    //unreachable
    *out_idx = HASHT_NOT_FOUND;
    return HASHT_INVALID_TABLE_STATE;
}

//fwddecl
static int hasht_init_copy_settings(struct hasht *ht, long initial_nelements, const struct hasht *source);
static int hasht_insert(struct hasht *ht, hasht_key_type *key, hasht_value_type *value);

static bool hasht_index_within(long start_idx, long cursor_idx, long end_idx_inclusive) {
    if ((start_idx <= end_idx_inclusive && cursor_idx >  end_idx_inclusive                             ) ||
        (start_idx >  end_idx_inclusive && cursor_idx <=  start_idx && cursor_idx >  end_idx_inclusive)  ||
        (cursor_idx == start_idx)                                                                           )
        return false;
    return true;
}
//returns a negative value if it finds none
//in first iteration cursor_idx must be HASHT_ITER_FIRST, this is used to tell the difference between whether we wrapped around or not
static long hasht_skip_to_next__(struct hasht *ht, long start_idx, long cursor_idx, long end_idx_inclusive) {
    if (ht->nelements == 0) {
        return HASHT_ITER_STOP;
    }
    else if (cursor_idx == HASHT_ITER_FIRST) {
        cursor_idx = start_idx;
    }
    else {
        cursor_idx = hasht_idx_mod_buckets(ht, cursor_idx + 1); 
        if (!hasht_index_within(start_idx, cursor_idx, end_idx_inclusive))
            return HASHT_ITER_STOP;
    }

    HASHT_ASSERT(end_idx_inclusive >= 0 && end_idx_inclusive < ht->nbuckets, "");
    HASHT_ASSERT(cursor_idx >= 0  &&  cursor_idx < ht->nbuckets, "");
    HASHT_ASSERT(start_idx >= 0  &&  start_idx < ht->nbuckets, "");
    for (long i=0; i<ht->nbuckets; i++) {
        struct hasht_pair_type *pair = ht->tab + cursor_idx;
        if (hasht_pr_is_occupied(pair)) {
            return cursor_idx;
        }
        cursor_idx = hasht_idx_mod_buckets(ht, cursor_idx + 1); 
        if (!hasht_index_within(start_idx, cursor_idx, end_idx_inclusive))
            return HASHT_ITER_STOP;
    }
    return HASHT_INVALID_TABLE_STATE;
}

static int hasht_copy_all_to(struct hasht *destination, struct hasht *source) {
    HASHT_ASSERT(destination != source && source && destination, "");
    int rv;
    long idx = hasht_skip_to_next__(source, 0, HASHT_ITER_FIRST, source->nbuckets - 1);
    while (idx >= 0) {
        struct hasht_pair_type *pair = source->tab + idx;
        rv = hasht_insert(destination, &pair->key, &pair->value);
        if (rv != HASHT_OK)
            return rv; //failed in middle of copying
        idx = hasht_skip_to_next__(source, 0, idx, source->nbuckets - 1);
    }
    if (idx != HASHT_ITER_STOP)
        return (int) idx; //failed somehow
    return HASHT_OK;
}

static int hasht_resize__(struct hasht *ht, long new_element_count) {

    long new_bucket_count = hasht_calc_nelements_to_nbuckets(new_element_count, ht->shrink_at_percentage, ht->grow_at_percentage);
    if (ht->nbuckets_po2 == hasht_get_adiv_power_idx(new_bucket_count)) {
        return HASHT_OK; 
        //because we use primes, for some reason both new value and old values map to the same power of two
        //and there is no point in resizing, since this is an approximate thing it's not a big deal
    }
    HASHT_ASSERT(new_bucket_count > HASHT_MIN_TABLESIZE, "");

    struct hasht new_ht;
    int rv = hasht_init_copy_settings(&new_ht, new_bucket_count, ht);
    if (rv != HASHT_OK) {
        return rv;
    }
    rv = hasht_copy_all_to(&new_ht, ht);
    if (rv != HASHT_OK) {
        hasht_deinit(&new_ht);
        return rv;
    }
    HASHT_ASSERT(new_ht.nelements == ht->nelements, "copying failed");
    HASHT_ASSERT(new_ht.ndeleted == 0, "copying failed");

    //swap and deinit
    hasht_deinit(ht);
    memcpy(ht, &new_ht, sizeof *ht);

    HASHT_ASSERT(hasht_dbg_sanity_heavy(ht), "");

    return HASHT_OK;
}

enum hasht_hint {
    HASHT_HINT_NONE,
    HASHT_HINT_INSERTING,
    HASHT_HINT_DELETING,
};
static int hasht_if_needed_try_resize(struct hasht *ht, int hint) {
    int rv = HASHT_OK;
    //avoids trying to shrink when we're inserting, and avoids trying to grow when we're removing elements
    if (ht->nelements >= ht->grow_at_gt_n && (hint != HASHT_HINT_DELETING)) {
        rv = hasht_resize__(ht, ht->nelements);
    }
    else if ((ht->nelements < ht->shrink_at_lt_n) && ((ht->nbuckets / 2) < HASHT_MIN_TABLESIZE) && (hint != HASHT_HINT_INSERTING)) {
        rv = hasht_resize__(ht, ht->nelements);
    }
    return rv;
}
static bool hasht_at_insert_must_resize(struct hasht *ht) {
    //we need to have at least one empty bucket, otherwise we can run into an infinite loop while searching
    return (hasht_n_unused_buckets(ht) <= 1); 
}

//clears flags, makes it occupied, copies key and value to it
static int hasht_set_pair_at_pos__(struct hasht *ht, size_t full_hash, hasht_key_type *key, hasht_value_type *value, long place_to_insert_idx) {
    HASHT_ASSERT(ht->nelements < ht->nbuckets, "");
    HASHT_ASSERT(place_to_insert_idx >= 0 && place_to_insert_idx < ht->nbuckets , "");
    struct hasht_pair_type *pair = ht->tab + place_to_insert_idx;
    pair->pair_data = hasht_pr_combine_flags_and_partial_hash(HASHT_VLT_IS_NOT_EMPTY, //flags
                                                        hasht_hash_to_partial_hash(full_hash));
    memcpy(&pair->key, key, sizeof *key);
    memcpy(&pair->value, value, sizeof *value);
    return HASHT_OK;
}

static int hasht_insert__(struct hasht *ht, hasht_key_type *key, hasht_value_type *value, long *found_idx_out, bool or_replace) {
    HASHT_ASSERT(hasht_dbg_sanity_01(ht), "hasht corrupt or not initialized");
    HASHT_ASSERT(ht->nelements < ht->nbuckets, "");
    HASHT_ASSERT(found_idx_out, "");
    long found_idx;
    int rv = hasht_if_needed_try_resize(ht, HASHT_HINT_INSERTING);
    if (rv != HASHT_OK && hasht_at_insert_must_resize(ht)) {
        //failed, translate the error
        if (rv == HASHT_INVALID_TABLE_STATE || rv == HASHT_ALLOC_ERR)
            return rv;
        else
            return HASHT_FAILED_AT_RESIZE;
    }

    size_t full_hash;
    rv = hasht_find_pos__(ht, key, &found_idx, &full_hash);

    if (found_idx == HASHT_NOT_FOUND) {
        //weird error, we were expecting either:
        //an index (if found)
        //a suggestion index (if not found)
        *found_idx_out = HASHT_NOT_FOUND;
        return HASHT_INVALID_TABLE_STATE;
    }

    if (rv == HASHT_OK && !or_replace) {
        //found duplicate, and we're not told to replace it
        *found_idx_out = found_idx;
        return HASHT_DUPLICATE_KEY;
    }
    else if (rv == HASHT_NOT_FOUND) {
        //not a duplicate, new element
        struct hasht_pair_type *pair = ht->tab + found_idx; 
        if (hasht_pr_is_deleted(pair)) {
            HASHT_ASSERT(ht->ndeleted > 0, "found a deleted element even though ht->ndeleted <= 0");
            ht->ndeleted--;
        }
        ht->nelements++;
    }
    else if (rv != HASHT_OK) {
        //some other error, return it
        *found_idx_out = HASHT_NOT_FOUND;
        return rv;
    }

    rv = hasht_set_pair_at_pos__(ht, full_hash, key, value, found_idx);
    *found_idx_out = found_idx;
    return rv;
}


//this only happens when we delete an element where the one next to it is empty:
// [filled] [filled] [filled and to be deleted] [empty]
//                    ^^^mark as empty^^^^^^     ^next^
//this _doesnt_ happen when we delete an element where the one next to it is filled or deleted:
// [filled] [filled] [filled and to be deleted] [filled or deleted] [filled] [empty]
//                    ^^^mark as deleted^^^^     ^next^
static void hasht_mark_as_empty__(struct hasht *ht, long at_index) {
    struct hasht_pair_type *pair = ht->tab + at_index; 
    HASHT_ASSERT(!hasht_pr_is_empty(pair), "");
    hasht_pr_set_flags(pair,
                  (hasht_pr_flags(pair) & (~ (HASHT_VLT_IS_NOT_EMPTY | HASHT_VLT_IS_DELETED))));
    HASHT_ASSERT(hasht_pr_is_empty(pair), "");
}
static void hasht_mark_as_occupied__(struct hasht *ht, long at_index) {
    struct hasht_pair_type *pair = ht->tab + at_index; 
    HASHT_ASSERT(hasht_pr_is_empty(pair) || hasht_pr_is_deleted(pair), "");
    hasht_pr_set_flags(pair,
                  (hasht_pr_flags(pair) & (~HASHT_VLT_IS_DELETED)) | HASHT_VLT_IS_NOT_EMPTY);
    HASHT_ASSERT(!hasht_pr_is_empty(pair), "");
}
static void hasht_mark_as_deleted__(struct hasht *ht, long at_index) {
    struct hasht_pair_type *pair = ht->tab + at_index; 
    HASHT_ASSERT(hasht_pr_is_occupied(pair), "trying to delete an empty element");
    hasht_pr_set_flags(pair,
                  hasht_pr_flags(pair) | HASHT_VLT_IS_DELETED);
    HASHT_ASSERT(hasht_pr_is_deleted(pair), "");
}

static int hasht_remove(struct hasht *ht, hasht_key_type *key) {
    long found_idx;
    size_t full_hash;
    int rv = hasht_find_pos__(ht, key, &found_idx, &full_hash);
    if (rv == HASHT_NOT_FOUND) {
        return rv;
    }
    else if (rv != HASHT_OK) {
        //failed, TODO: check what's the error
        return rv;
    }
    HASHT_ASSERT(found_idx >= 0 && found_idx < ht->nbuckets, "find pos returned invalid index");
#ifdef HASHT_DBG
        struct hasht_pair_type *pair = ht->tab + found_idx;
        HASHT_ASSERT(hasht_pr_is_occupied(pair), "find pos returned an index of a deleted/empty element");
#endif // HASHT_DBG

    //optimization: if next element is empty, mark our element as empty too, otherwise mark our element as deleted
    //TODO: benchmark this
    long next_idx = hasht_idx_mod_buckets(ht, found_idx + 1); //this assumes linear probing
    struct hasht_pair_type *next_pair = ht->tab + next_idx;
    //TODO, division even though it is fast can be optimized to be a branch in wrap around cases
    //After benchmarking this, the results were: cleaning up in general made things faster by 2.0%
    //HASHT_AGRESSIVE_CLEANUP made things faster by about 0.5% (which is insignificant)
    if (hasht_pr_is_empty(next_pair)) {
        hasht_mark_as_empty__(ht, found_idx);

        //^TODO: add tests that extensively test the table state after lots of deletions
        //
        //when we have the situation:
        //                          [deleted] [deleted] [deleted] [filled]                      [empty] [filled] [empty] [deleted]
        //                               ^        ^       ^         ^the item we just deleted    ^        ^^^dont care^^^^^^^^^^
        //                               ^^^^^^^^^^^^^^^^^^^                                     ^must be empty
        //                             ^^^^it doesn't matter to what hashes those keys were originally from (their probe length)
        //                             ^^^^as long as they're deleted (And followd by an empty bucket)
        //                             ^^^^we can mark them empty
        //
        //
        //what was                  [deleted] [deleted] [deleted] [filled] [empty]
        //becomes:                  [deleted] [deleted] [deleted] [empty]  [empty]
        //                          [deleted] [deleted] [empty]   [empty]  [empty]
        //                          [deleted] [empty]   [empty]   [empty]  [empty]
        //                          [empty]   [empty]   [empty]   [empty]  [empty]
        //this an attempt to speed up searches after deletions
        
        #define HASHT_AGRESSIVE_CLEANUP
        #ifdef HASHT_AGRESSIVE_CLEANUP
            long prev_idx = hasht_idx_mod_buckets(ht, found_idx - 1);
            struct hasht_pair_type *prev_pair = ht->tab + prev_idx;
            while (hasht_pr_is_deleted(prev_pair)) {
                hasht_mark_as_empty__(ht, prev_idx);
                HASHT_ASSERT(hasht_pr_is_empty(prev_pair), "");
                prev_idx = hasht_idx_mod_buckets(ht, prev_idx - 1); 
                prev_pair = ht->tab + prev_idx;
            }
        #else
            long supposed_to_be_in_idx = hasht_integer_mod_buckets(ht, full_hash);
            long probe_len = found_idx - supposed_to_be_in_idx;
            if (probe_len < 0)
                probe_len = probe_len + ht->nbuckets;
            long prev_idx = hasht_idx_mod_buckets(ht, found_idx - 1);
            struct hasht_pair_type *prev_pair = ht->tab + prev_idx;
            for (long i = 0; i < probe_len && hasht_pr_is_deleted(prev_pair); i++) {
                hasht_mark_as_empty__(ht, prev_idx);
                HASHT_ASSERT(hasht_pr_is_empty(prev_pair), "");
                prev_idx = hasht_idx_mod_buckets(ht, prev_idx - 1); 
                prev_pair = ht->tab + prev_idx;
            }
        #endif // HASHT_AGRESSIVE_CLEANUP
    }
    else {
        hasht_mark_as_deleted__(ht, found_idx);
        ht->ndeleted++;
    }

    ht->nelements--;
    return HASHT_OK;
}
static int hasht_insert(struct hasht *ht, hasht_key_type *key, hasht_value_type *value) {
    long idx_unused;
    int rv = hasht_insert__(ht, key, value, &idx_unused, false /*dont replace*/);
    return rv;
}
struct hasht_iter {
    long started_at_idx;
    long current_idx;
    //public field
    //the two members: pair->key and pair->value can be accessed directly (assuming a valid iterator)
    struct hasht_pair_type *pair; 
};
static struct hasht_iter hasht_mk_invalid_iter(void) {
    struct hasht_iter iter = {HASHT_ITER_STOP, HASHT_ITER_STOP, NULL};
    return iter;
}
static struct hasht_iter hasht_mk_iter(long start_idx, struct hasht_pair_type *pair) {
    struct hasht_iter iter = {start_idx, HASHT_ITER_FIRST, pair};
    return iter;
}
static bool hasht_iter_check(struct hasht_iter *iter) {
    HASHT_ASSERT((iter->current_idx == HASHT_ITER_STOP) || (iter->pair != NULL && iter->current_idx >= 0), "invalid iterator state");
    return iter->current_idx != HASHT_ITER_STOP;
}

static int hasht_begin_iterator(struct hasht *ht, struct hasht_iter *iter) {
    long next_idx = hasht_skip_to_next__(ht, 0, HASHT_ITER_FIRST, ht->nbuckets - 1);
    if (next_idx < 0) {
        *iter = hasht_mk_invalid_iter();
        return HASHT_ITER_STOP;
    }
    iter->started_at_idx = 0;
    iter->current_idx = next_idx;
    iter->pair = ht->tab + next_idx;
    return HASHT_OK;
}

static int hasht_iter_next(struct hasht *ht, struct hasht_iter *iter) {
    HASHT_ASSERT((iter->current_idx == HASHT_ITER_FIRST) ||
                 (iter->current_idx >= 0 && iter->current_idx < ht->nbuckets), "invalid iterator");

    if (iter->current_idx == HASHT_ITER_STOP)
        return HASHT_ITER_STOP; //the caller will probably be stuck in an infinite loop, that's what you get for not checking return value

    long next_idx = hasht_skip_to_next__(ht, iter->started_at_idx, iter->current_idx, ht->nbuckets - 1);
    if (next_idx < 0) {
        *iter = hasht_mk_invalid_iter();
        return HASHT_ITER_STOP;
    }
    iter->current_idx = next_idx;
    iter->pair = ht->tab + next_idx;
    return HASHT_OK;
}
static int hasht_find(struct hasht *ht, hasht_key_type *key, struct hasht_iter *out) {
    long found_idx;
    size_t full_hash_unused;
    int rv = hasht_find_pos__(ht, key, &found_idx, &full_hash_unused);
    if (rv == HASHT_NOT_FOUND) {
        *out = hasht_mk_invalid_iter();
        return rv;
    }
    else if (rv != HASHT_OK) {
        //failed, TODO: check what's the error
        *out = hasht_mk_invalid_iter();
        return rv;
    }
    HASHT_ASSERT(found_idx >= 0 && found_idx < ht->nbuckets, "find pos returned invalid index");
    struct hasht_pair_type *pair = ht->tab + found_idx;
    *out = hasht_mk_iter(found_idx, pair);
    return HASHT_OK;
}
static int hasht_find_or_insert(struct hasht *ht, hasht_key_type *key, hasht_value_type *value, struct hasht_iter *out) {
    long found_idx;
    int rv = hasht_insert__(ht, key, value, &found_idx, true /*do replace*/);
    if (rv == HASHT_OK) {
        struct hasht_pair_type *pair = ht->tab + found_idx;
        *out = hasht_mk_iter(found_idx, pair);
    }
    else {
        *out = hasht_mk_invalid_iter();
    }
    return rv;
}


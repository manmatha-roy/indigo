/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

/**
 * @file
 * @brief Interface for OFStateManager flow table module
 *
 * Normally, the flow table is used in the following way:
 *
 * Create an instance based on fixed or external configuration
 * For each flow add that comes in:
 *    Create a new entry in the local table in a "creating" state
 *    Push the flow add across the HAL boundary
 *    When the successful response comes back
 *        Change the state in the local table to "stable"
 *
 * Usage Conventions
 *
 * The flow table entry structure is published here.  It has a pointer
 * to the original flow add.  After an implementation completes the "add"
 * operation, it should never refer to that pointer.
 *
 * The flow table is logically a database with a single primary key, the
 * integer flow ID.  This value is determined by the client of the flow
 * table instance.
 *
 * When a client receives a reference to a flow table entry, it must
 * treat the entire structure as read-only.
 */

#ifndef _OFSTATEMANAGER_FT_H_
#define _OFSTATEMANAGER_FT_H_

#include <indigo/indigo.h>
#include <indigo/fi.h>
#include <loci/loci.h>
#include <BigList/biglist.h>
#include <AIM/aim_list.h>
#include <stdbool.h>
#include <debug_counter/debug_counter.h>
#include <BigHash/bighash.h>

#include "ft_entry.h"

#define FT_MAX_TABLES 32

/**
 * Length of the prefix used for bucketing flows by cookie.
 */
#define FT_COOKIE_PREFIX_LEN 8
#define FT_COOKIE_PREFIX_MASK (~(uint64_t)0 << (64-FT_COOKIE_PREFIX_LEN))

/**
 * Forward declaration of flowtable handle for other typedefs
 */

typedef struct ft_public_s ft_public_t;

/**
 * A handle is a pointer to an instance.
 */

typedef ft_public_t *ft_instance_t;

typedef struct ft_cookie_bucket_s {
    list_head_t head;
} ft_cookie_bucket_t;

/**
 * Per-table bookkeeping
 *
 * The checksum buckets are used for the bsn_flow_checksum extension. Flows are
 * bucketed by checksum prefix and their cookies XORed into the bucket. The
 * per-table checksum field is the XOR of the cookies of every flow in the
 * table.
 */

typedef struct ft_table_s {
    uint64_t checksum;
    int checksum_buckets_size;
    int checksum_shift;
    uint64_t *checksum_buckets;
} ft_table_t;

/**
 * The public view of the instance for easier dereference
 *
 * This should be treated as read-only outside of the
 * flow table instance implementation
 */
struct ft_public_s {
    int current_count;             /* Number of flows in the flowtable */

    list_head_t all_list;          /* Single list of all current entries */

    bighash_table_t *strict_match_hashtable;
    ft_cookie_bucket_t *cookie_buckets;   /* Array of cookie (prefix) based buckets */

    ft_table_t tables[FT_MAX_TABLES];
};

/**
 * Safe iterator for the flowtable
 *
 * See ft_iterator_init, ft_iterator_next, and ft_iterator_cleanup.
 *
 * This struct should be treated as opaque.
 */
typedef struct ft_iterator_s {
    list_head_t *head;             /* List head for this iteration */
    ft_entry_t *next_entry;        /* Entry to be returned on next() */
    int links_offset;              /* Offset of the links we're using in the flowtable entry */
    list_links_t entry_links;      /* Linked into next_entry->iterators if next_entry != NULL */
    bool use_query;                /* Whether 'query' is valid */
    of_meta_match_t query;         /* Optional query to filter by */
} ft_iterator_t;

/**
 * Safe iterator for entire flow table
 *
 * The current entry may be deleted during this iteration.
 * @param _ft The instance of the flow table being iterated
 * @param _entry Pointer to the "current" entry in the iteration
 * @param _cur list_link_t bookkeeping pointer, do not reference
 * @param _next list_link_t bookkeeping pointer, do not refernece
 *
 * Assumes the ft_instance is initialized
 */

#define FT_ITER(_ft, _entry, _cur, _next)                               \
    if (!list_empty(&(_ft)->all_list))                                  \
        for ((_cur) = (_ft)->all_list.links.next,                       \
                 _entry = FT_ENTRY_CONTAINER(_cur, table);              \
             _next = _cur->next, _cur != &((_ft)->all_list.links);      \
             _cur = _next, _entry = FT_ENTRY_CONTAINER((_cur), table))

/*
 * Create a flow table instance
 *
 * @returns A handle for the flow table instance
 */

ft_instance_t ft_create(void);

/**
 * Delete a flow table instance and free resources
 * @param ft A handle for the flow table instance to be deleted
 *
 * Will call ft_entry_clear on all entries.
 *
 * Free underlying data structures
 */

void ft_destroy(ft_instance_t ft);

/**
 * Add a flow entry to the table
 * @param ft The flow table handle
 * @param id The external flow identifier
 * @param flow_add The LOCI flow mod object resulting in the add
 * @param minimatch Pointer to the minimatch extracted from the flow
 * @param entry_p Output; pointer to place to store entry if successful
 *
 * If the entry already exists, an error is returned.
 *
 * The minimatch is moved.
 */

indigo_error_t ft_add(ft_instance_t ft,
                      indigo_flow_id_t id,
                      of_flow_add_t *flow_add,
                      minimatch_t *minimatch,
                      ft_entry_t **entry_p);

/**
 * Remove a specific flow entry from the table
 * @param ft The flow table handle
 * @param entry Pointer to the entry to be removed
 */

void ft_delete(ft_instance_t ft, ft_entry_t *entry);

/**
 * Overwrite an existing flow entry in the table
 * @param ft The flow table handle
 * @param entry Pointer to the entry to be overwritten
 * @param flow_add The LOCI flow mod object resulting in the overwrite
 *
 * This function updates the cookie, flags, timeouts, instructions, and
 * creation time, as specified in OpenFlow 1.3.3 section 6.4.
 */
void ft_overwrite(ft_instance_t ft, ft_entry_t *entry, of_flow_add_t *flow_add);

/**
 * Query the flow table (strict match) and return the first match if found
 * @param ft Handle for a flow table instance
 * @param query The meta-match data for the query
 * @param entry_ptr (out) Pointer to where to store the result if found
 * @returns INDIGO_ERROR_NONE if found; otherwise INDIGO_ERROR_NOT_FOUND
 */

indigo_error_t ft_strict_match(ft_instance_t instance,
                               of_meta_match_t *query,
                               ft_entry_t **entry_ptr);

/**
 * Resize the checksum buckets array for a table
 */
indigo_error_t
ft_set_checksum_buckets_size(ft_instance_t ft, uint8_t table_id, uint32_t buckets_size);

/**
 * Modify the effects of a flow entry in the table
 * @param ft The flow table handle
 * @param entry Pointer to the entry to update
 * @param flow_mod The LOCI flow mod object resulting in the modification
 *
 * The actions (instructions) and related metadata are updated for the flow
 */

indigo_error_t
ft_entry_modify_effects(ft_instance_t instance,
                        ft_entry_t *entry,
                        of_flow_modify_t *flow_mod);

/*
 * Spawn a task that iterates over the flowtable
 *
 * @param ft Handle for a flow table instance
 * @param query The meta-match data for the query (or NULL)
 * @param callback Function called for each flowtable entry
 * @returns An error code
 *
 * This function does not guarantee a consistent view of the
 * flowtable over the course of the task.
 *
 * This function does not use any indexes on the flowtable.
 *
 * The callback function will be called with a NULL entry argument at
 * the end of the iteration.
 *
 * This function moves the metamatch. The caller does not need to clean up the
 * metamatch, even if this function fails.
 *
 * Deleted entries are skipped.
 */

typedef void (*ft_iter_task_callback_f)(void *cookie, ft_entry_t *entry);

indigo_error_t
ft_spawn_iter_task(ft_instance_t instance,
                   of_meta_match_t *query,
                   ft_iter_task_callback_f callback,
                   void *cookie,
                   int priority);

/**
 * Initialize a flowtable iterator
 *
 * This will iterate over the flowtable, returning the subset matching 'query'
 * (or the entire flowtable if 'query' is NULL). It is safe to use with concurrent
 * modification of the flowtable.
 *
 * This iterator does not guarantee a consistent view of the flowtable over
 * the course of the iteration. Flows added during the iteration may or may
 * not be returned by the iterator.
 */
void
ft_iterator_init(ft_iterator_t *iter, ft_instance_t ft, of_meta_match_t *query);

/**
 * Yield the next entry from an iterator
 *
 * Will return NULL to signal the end of the iteration.
 */
ft_entry_t *
ft_iterator_next(ft_iterator_t *iter);

/**
 * Cleanup a flowtable iterator
 */
void
ft_iterator_cleanup(ft_iterator_t *iter);


/* Debug counters */
extern debug_counter_t ft_flow_counter;
extern debug_counter_t ft_add_counter;
extern debug_counter_t ft_delete_counter;
extern debug_counter_t ft_modify_counter;
extern debug_counter_t ft_forwarding_add_error_counter;

#endif /* _OFSTATEMANAGER_FT_H_ */

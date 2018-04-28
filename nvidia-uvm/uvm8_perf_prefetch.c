/*******************************************************************************
    Copyright (c) 2016 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN hint OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm_linux.h"
#include "uvm8_perf_events.h"
#include "uvm8_perf_module.h"
#include "uvm8_perf_prefetch.h"
#include "uvm8_kvmalloc.h"
#include "uvm8_va_block.h"
#include "uvm8_va_range.h"

// Global cache to allocate the per-VA block prefetch detection structures
static struct kmem_cache *g_prefetch_info_cache __read_mostly;

// Per-VA block prefetch detection structure
typedef struct
{
    DECLARE_BITMAP(prefetch_pages, PAGES_PER_UVM_VA_BLOCK);

    DECLARE_BITMAP(migrate_pages, PAGES_PER_UVM_VA_BLOCK);

    uvm_va_block_bitmap_tree_t bitmap_tree;

    uvm_processor_id_t last_migration_proc_id;

    NvU16 first_page;

    NvU16 outer_page;

    size_t big_page_size;

    uvm_va_block_region_t big_pages_region;

    NvU16 pending_prefetch_pages;

    NvU16 fault_migrations_to_last_proc;
} block_prefetch_info_t;

//
// Tunables for prefetch detection/prevention (configurable via module parameters)
//

// Enable/disable prefetch performance heuristics
static unsigned uvm_perf_prefetch_enable = 1;

// TODO: Bug 1778037: [uvm8] Use adaptive threshold for page prefetching
#define UVM_PREFETCH_THRESHOLD_DEFAULT 51

// Percentage of children subregions that need to be resident in order to
// trigger prefetching of the remaining subregions
//
// Valid values 1-100
static unsigned uvm_perf_prefetch_threshold  = UVM_PREFETCH_THRESHOLD_DEFAULT;

#define UVM_PREFETCH_MIN_FAULTS_MIN     1
#define UVM_PREFETCH_MIN_FAULTS_DEFAULT 1
#define UVM_PREFETCH_MIN_FAULTS_MAX     20

// Minimum number of faults on a block in order to enable the prefetching
// logic
static unsigned uvm_perf_prefetch_min_faults = UVM_PREFETCH_MIN_FAULTS_DEFAULT;

// Whether to cross range group range boundaries when prefetching
static unsigned uvm_perf_prefetch_across_range_group_ranges = false;

// Module parameters for the tunables
module_param(uvm_perf_prefetch_enable, uint, S_IRUGO);
module_param(uvm_perf_prefetch_threshold, uint, S_IRUGO);
module_param(uvm_perf_prefetch_min_faults, uint, S_IRUGO);

// TODO: Bug 1872604: Remove module parameter when the bug is fixed
module_param(uvm_perf_prefetch_across_range_group_ranges, uint, S_IRUGO);

static bool g_uvm_perf_prefetch_enable;
static unsigned g_uvm_perf_prefetch_threshold;
static unsigned g_uvm_perf_prefetch_min_faults;
static bool g_uvm_perf_prefetch_across_range_group_ranges;

// Callback declaration for the performance heuristics events
static void prefetch_block_destroy_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);

static uvm_va_block_region_t compute_prefetch_region(size_t page_index, block_prefetch_info_t *prefetch_info)
{
    NvU16 counter;
    uvm_va_block_bitmap_tree_iter_t iter;
    uvm_va_block_bitmap_tree_t *bitmap_tree = &prefetch_info->bitmap_tree;
    uvm_va_block_region_t prefetch_region = uvm_va_block_region(bitmap_tree->leaf_count,
                                                                bitmap_tree->leaf_count + 1);

    uvm_va_block_bitmap_tree_traverse_counters(counter, bitmap_tree, page_index, &iter) {
        uvm_va_block_region_t subregion = uvm_va_block_bitmap_tree_iter_get_range(bitmap_tree, &iter);
        NvU16 subregion_pages = uvm_va_block_region_num_pages(subregion);

        UVM_ASSERT(counter <= subregion_pages);
        if (counter * 100 > subregion_pages * g_uvm_perf_prefetch_threshold)
            prefetch_region = subregion;
    }

    // Clamp prefetch region to actual pages
    if (prefetch_region.first < bitmap_tree->leaf_count) {
        if (prefetch_region.first < prefetch_info->first_page)
            prefetch_region.first = prefetch_info->first_page;

        if (prefetch_region.outer > prefetch_info->outer_page)
            prefetch_region.outer = prefetch_info->outer_page;
    }

    return prefetch_region;
}

// Performance heuristics module for prefetch
static uvm_perf_module_t g_module_prefetch;

static uvm_perf_module_event_callback_desc_t g_callbacks_prefetch[] = {
    { UVM_PERF_EVENT_BLOCK_DESTROY, prefetch_block_destroy_cb },
    { UVM_PERF_EVENT_MODULE_UNLOAD, prefetch_block_destroy_cb },
    { UVM_PERF_EVENT_BLOCK_SHRINK,  prefetch_block_destroy_cb }
};

// Get the prefetch detection struct for the given block
static block_prefetch_info_t *prefetch_info_get(uvm_va_block_t *va_block)
{
    return uvm_perf_module_type_data(va_block->perf_modules_data, UVM_PERF_MODULE_TYPE_PREFETCH);
}

static void prefetch_info_destroy(uvm_va_block_t *va_block)
{
    block_prefetch_info_t *prefetch_info = prefetch_info_get(va_block);
    if (prefetch_info) {
        kmem_cache_free(g_prefetch_info_cache, prefetch_info);
        uvm_perf_module_type_unset_data(va_block->perf_modules_data, UVM_PERF_MODULE_TYPE_PREFETCH);
    }
}

// Get the prefetch detection struct for the given block or create it if it
// does not exist
static block_prefetch_info_t *prefetch_info_get_create(uvm_va_block_t *va_block)
{
    block_prefetch_info_t *prefetch_info = prefetch_info_get(va_block);
    if (!prefetch_info) {
        // Create some ghost leaves so we can align the tree to big page boundary. We use the
        // largest page size to handle the worst-case scenario
        size_t big_page_size = UVM_PAGE_SIZE_128K;
        uvm_va_block_region_t big_pages_region = uvm_va_block_big_page_region_all(va_block, big_page_size);
        size_t num_leaves = uvm_va_block_num_cpu_pages(va_block);

        // If the va block is not big enough to fit 128KB pages, maybe it still can fit 64KB pages
        if (big_pages_region.outer == 0) {
            big_page_size    = UVM_PAGE_SIZE_64K;
            big_pages_region = uvm_va_block_big_page_region_all(va_block, big_page_size);
        }

        if (big_pages_region.first > 0)
            num_leaves += (big_page_size / PAGE_SIZE - big_pages_region.first);

        UVM_ASSERT(num_leaves <= PAGES_PER_UVM_VA_BLOCK);

        prefetch_info = kmem_cache_zalloc(g_prefetch_info_cache, NV_UVM_GFP_FLAGS);
        if (!prefetch_info)
            goto fail;

        prefetch_info->last_migration_proc_id = UVM8_MAX_PROCESSORS;

        uvm_va_block_bitmap_tree_init_from_page_count(&prefetch_info->bitmap_tree, num_leaves);

        uvm_perf_module_type_set_data(va_block->perf_modules_data, prefetch_info, UVM_PERF_MODULE_TYPE_PREFETCH);
    }

    return prefetch_info;

fail:
    prefetch_info_destroy(va_block);

    return NULL;
}

static void grow_fault_granularity_if_no_thrashing(block_prefetch_info_t *prefetch_info,
                                                   uvm_va_block_region_t region,
                                                   const long unsigned *faulted_pages,
                                                   const long unsigned *thrashing_pages)
{
    if (!uvm_page_mask_region_empty(faulted_pages, region) &&
        (!thrashing_pages || uvm_page_mask_region_empty(thrashing_pages, region))) {
        region.first += prefetch_info->first_page;
        region.outer += prefetch_info->first_page;
        uvm_page_mask_region_fill(prefetch_info->bitmap_tree.pages, region);
    }
}

static void grow_fault_granularity(uvm_va_block_t *va_block,
                                   block_prefetch_info_t *prefetch_info,
                                   const long unsigned *faulted_pages,
                                   const long unsigned *thrashing_pages)
{
    size_t num_big_pages;
    size_t big_page_index;
    uvm_va_block_region_t block_region = uvm_va_block_region_from_block(va_block);

    // Migrate whole "prefix" if no page in it is thrashing
    if (prefetch_info->big_pages_region.first > 0) {
        uvm_va_block_region_t prefix_region = uvm_va_block_region(0, prefetch_info->big_pages_region.first);

        grow_fault_granularity_if_no_thrashing(prefetch_info, prefix_region, faulted_pages, thrashing_pages);
    }

    // Migrate whole big pages if they are not thrashing
    num_big_pages = uvm_va_block_num_big_pages(va_block, prefetch_info->big_page_size);
    for (big_page_index = 0; big_page_index < num_big_pages; ++big_page_index) {
        uvm_va_block_region_t big_region = uvm_va_block_big_page_region(va_block,
                                                                        big_page_index,
                                                                        prefetch_info->big_page_size);

        grow_fault_granularity_if_no_thrashing(prefetch_info, big_region, faulted_pages, thrashing_pages);
    }

    // Migrate whole "suffix" if no page in it is thrashing
    if (prefetch_info->big_pages_region.outer < block_region.outer) {
        uvm_va_block_region_t suffix_region = uvm_va_block_region(prefetch_info->big_pages_region.outer,
                                                                  block_region.outer);

        grow_fault_granularity_if_no_thrashing(prefetch_info, suffix_region, faulted_pages, thrashing_pages);
    }
}

// Within a block we only allow prefetching to a single processor. Therefore, if two processors
// are accessing non-overlapping regions within the same block they won't benefit from
// prefetching.
//
// TODO: Bug 1778034: [uvm8] Explore prefetching to different processors within a VA block
void uvm_perf_prefetch_prenotify_fault_migrations(uvm_va_block_t *va_block,
                                                  uvm_processor_id_t new_residency,
                                                  const long unsigned *faulted_pages,
                                                  uvm_va_block_region_t region)
{
    size_t page_index;
    block_prefetch_info_t *prefetch_info;
    const long unsigned *resident_mask = NULL;
    const long unsigned *thrashing_pages = NULL;
    uvm_va_range_t *va_range = va_block->va_range;

    if (!g_uvm_perf_prefetch_enable)
        return;

    prefetch_info = prefetch_info_get_create(va_block);
    if (!prefetch_info)
        return;

    if (prefetch_info->last_migration_proc_id != new_residency) {
        prefetch_info->last_migration_proc_id = new_residency;
        prefetch_info->fault_migrations_to_last_proc = 0;
    }

    prefetch_info->pending_prefetch_pages = 0;

    // If this is a first-touch fault and the faulting processor is the preferred
    // location, populate the whole VA block
    if (uvm_processor_mask_empty(&va_block->resident) && new_residency == va_range->preferred_location) {
        uvm_page_mask_region_fill(prefetch_info->prefetch_pages, uvm_va_block_region_from_block(va_block));
        goto done;
    }

    if (new_residency == UVM_CPU_ID || va_block->gpus[new_residency - 1] != NULL)
        resident_mask = uvm_va_block_resident_mask_get(va_block, new_residency);

    if (resident_mask)
        uvm_page_mask_or(prefetch_info->bitmap_tree.pages, resident_mask, faulted_pages);
    else
        uvm_page_mask_copy(prefetch_info->bitmap_tree.pages, faulted_pages);

    // Get the big page size for the new residency
    if (new_residency != UVM_CPU_ID) {
        uvm_gpu_t *gpu = uvm_gpu_get(new_residency);
        prefetch_info->big_page_size = uvm_va_block_gpu_big_page_size(va_block, gpu);
    }
    else {
        prefetch_info->big_page_size = UVM_PAGE_SIZE_64K;
    }

    // Adjust the prefetch tree to big page granularity to make sure that we get big page-friendly
    // prefetching hints
    prefetch_info->big_pages_region = uvm_va_block_big_page_region_all(va_block, prefetch_info->big_page_size);
    if (prefetch_info->big_pages_region.first > 0) {
        prefetch_info->first_page = prefetch_info->big_page_size / PAGE_SIZE - prefetch_info->big_pages_region.first;

        bitmap_shift_left(prefetch_info->bitmap_tree.pages,
                          prefetch_info->bitmap_tree.pages,
                          prefetch_info->first_page,
                          PAGES_PER_UVM_VA_BLOCK);
    }
    else {
        prefetch_info->first_page = 0;
    }

    prefetch_info->outer_page = prefetch_info->first_page + uvm_va_block_num_cpu_pages(va_block);

    thrashing_pages = uvm_perf_thrashing_get_thrashing_pages(va_block);

    // Assume big pages by default. Prefetch the rest of 4KB subregions within the big page
    // region unless there is thrashing.
    grow_fault_granularity(va_block, prefetch_info, faulted_pages, thrashing_pages);

    // Do not compute prefetch regions with faults on pages that are thrashing
    if (thrashing_pages)
        uvm_page_mask_andnot(prefetch_info->migrate_pages, faulted_pages, thrashing_pages);
    else
        uvm_page_mask_copy(prefetch_info->migrate_pages, faulted_pages);

    // Update the tree using the migration mask to compute the pages to prefetch
    uvm_page_mask_zero(prefetch_info->prefetch_pages);
    for_each_va_block_page_in_mask(page_index, prefetch_info->migrate_pages, region) {
        uvm_va_block_region_t prefetch_region = compute_prefetch_region(page_index + prefetch_info->first_page,
                                                                        prefetch_info);
        uvm_page_mask_region_fill(prefetch_info->prefetch_pages, prefetch_region);

        // Early out if we have already prefetched until the end of the VA block
        if (prefetch_region.outer == prefetch_info->outer_page)
            break;
    }

    // Adjust prefetching page mask
    if (prefetch_info->first_page > 0) {
        bitmap_shift_right(prefetch_info->prefetch_pages,
                           prefetch_info->prefetch_pages,
                           prefetch_info->first_page,
                           PAGES_PER_UVM_VA_BLOCK);
    }

done:
    // Do not prefetch pages that are going to be migrated due to a fault or are already resident in
    // the destination processor
    uvm_page_mask_andnot(prefetch_info->prefetch_pages,
                         prefetch_info->prefetch_pages,
                         faulted_pages);
    if (resident_mask) {
        uvm_page_mask_andnot(prefetch_info->prefetch_pages,
                             prefetch_info->prefetch_pages,
                             resident_mask);
    }

    // Avoid prefetching pages that are thrashing
    if (thrashing_pages) {
        uvm_page_mask_andnot(prefetch_info->prefetch_pages,
                             prefetch_info->prefetch_pages,
                             thrashing_pages);
    }

    prefetch_info->fault_migrations_to_last_proc += uvm_page_mask_region_weight(faulted_pages, region);
    prefetch_info->pending_prefetch_pages = uvm_page_mask_weight(prefetch_info->prefetch_pages);
}

uvm_perf_prefetch_hint_t uvm_perf_prefetch_get_hint(uvm_va_block_t *va_block,
                                                    const long unsigned *new_residency_mask)
{
    uvm_perf_prefetch_hint_t ret = UVM_PERF_PREFETCH_HINT_NONE();
    block_prefetch_info_t *prefetch_info;
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    if (!g_uvm_perf_prefetch_enable)
        return ret;

    if (!va_space->test_page_prefetch_enabled)
        return ret;

    prefetch_info = prefetch_info_get(va_block);
    if (!prefetch_info)
        return ret;

    if (prefetch_info->fault_migrations_to_last_proc >= g_uvm_perf_prefetch_min_faults &&
        prefetch_info->pending_prefetch_pages > 0) {
        bool changed = false;

        if (g_uvm_perf_prefetch_across_range_group_ranges) {
            uvm_range_group_range_iter_t iter;
            uvm_range_group_for_each_migratability_in(&iter, va_space, va_block->start, va_block->end) {
                if (!iter.migratable) {
                    uvm_va_block_region_t region = uvm_va_block_region_from_start_end(va_block, iter.start, iter.end);

                    if (!uvm_page_mask_region_empty(prefetch_info->prefetch_pages, region)) {
                        uvm_page_mask_region_clear(prefetch_info->prefetch_pages, region);
                        changed = true;
                    }
                }
            }
        }
        else {
            uvm_range_group_range_t *rgr;

            // Only prefetch in range group ranges which have pages that need to
            // move
            //
            // TODO: Bug 1872604: Investigate why data consistency is broken when
            // prefetching across range group range boundaries
            uvm_range_group_for_each_range_in(rgr, va_space, va_block->start, va_block->end) {
                uvm_va_block_region_t region = uvm_va_block_region_from_start_end(va_block,
                                                                                  max(rgr->node.start, va_block->start),
                                                                                  min(rgr->node.end, va_block->end));

                if (uvm_page_mask_region_empty(new_residency_mask, region) &&
                    !uvm_page_mask_region_empty(prefetch_info->prefetch_pages, region)) {
                    uvm_page_mask_region_clear(prefetch_info->prefetch_pages, region);
                    changed = true;
                }
            }
        }

        if (changed)
            prefetch_info->pending_prefetch_pages = uvm_page_mask_weight(prefetch_info->prefetch_pages);

        if (prefetch_info->pending_prefetch_pages > 0) {
            ret.residency = prefetch_info->last_migration_proc_id;
            ret.prefetch_pages_mask = prefetch_info->prefetch_pages;
        }
    }

    return ret;
}

void prefetch_block_destroy_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_block_t *va_block;

    UVM_ASSERT(g_uvm_perf_prefetch_enable);

    UVM_ASSERT(event_id == UVM_PERF_EVENT_BLOCK_DESTROY ||
               event_id == UVM_PERF_EVENT_MODULE_UNLOAD ||
               event_id == UVM_PERF_EVENT_BLOCK_SHRINK);

    if (event_id == UVM_PERF_EVENT_BLOCK_DESTROY)
        va_block = event_data->block_destroy.block;
    else if (event_id == UVM_PERF_EVENT_BLOCK_SHRINK)
        va_block = event_data->block_shrink.block;
    else
        va_block = event_data->module_unload.block;

    if (!va_block)
        return;

    prefetch_info_destroy(va_block);
}

NV_STATUS uvm_perf_prefetch_load(uvm_va_space_t *va_space)
{
    if (!g_uvm_perf_prefetch_enable)
        return NV_OK;

    return uvm_perf_module_load(&g_module_prefetch, va_space);
}

void uvm_perf_prefetch_unload(uvm_va_space_t *va_space)
{
    if (!g_uvm_perf_prefetch_enable)
        return;

    uvm_perf_module_unload(&g_module_prefetch, va_space);
}

NV_STATUS uvm_perf_prefetch_init()
{
    g_uvm_perf_prefetch_enable = uvm_perf_prefetch_enable != 0;

    if (!g_uvm_perf_prefetch_enable)
        return NV_OK;

    uvm_perf_module_init("perf_prefetch", UVM_PERF_MODULE_TYPE_PREFETCH, g_callbacks_prefetch,
                         ARRAY_SIZE(g_callbacks_prefetch), &g_module_prefetch);

    g_prefetch_info_cache = NV_KMEM_CACHE_CREATE("block_prefetch_info_t", block_prefetch_info_t);
    if (!g_prefetch_info_cache)
        return NV_ERR_NO_MEMORY;

    if (uvm_perf_prefetch_threshold <= 100) {
        g_uvm_perf_prefetch_threshold = uvm_perf_prefetch_threshold;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_prefetch_threshold. Using %u instead\n",
                uvm_perf_prefetch_threshold, UVM_PREFETCH_THRESHOLD_DEFAULT);

        g_uvm_perf_prefetch_threshold = UVM_PREFETCH_THRESHOLD_DEFAULT;
    }

    if (uvm_perf_prefetch_min_faults >= UVM_PREFETCH_MIN_FAULTS_MIN &&
        uvm_perf_prefetch_min_faults <= UVM_PREFETCH_MIN_FAULTS_MAX) {
        g_uvm_perf_prefetch_min_faults = uvm_perf_prefetch_min_faults;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_prefetch_min_faults. Using %u instead\n",
                uvm_perf_prefetch_min_faults, UVM_PREFETCH_MIN_FAULTS_DEFAULT);

        g_uvm_perf_prefetch_min_faults = UVM_PREFETCH_MIN_FAULTS_DEFAULT;
    }

    g_uvm_perf_prefetch_across_range_group_ranges = uvm_perf_prefetch_across_range_group_ranges != 0;

    return NV_OK;
}

void uvm_perf_prefetch_exit()
{
    if (!g_uvm_perf_prefetch_enable)
        return;

    kmem_cache_destroy_safe(&g_prefetch_info_cache);
}

NV_STATUS uvm8_test_set_page_prefetch_policy(UVM_TEST_SET_PAGE_PREFETCH_POLICY_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    if (params->policy >= UVM_TEST_PAGE_PREFETCH_POLICY_MAX)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_write(va_space);

    if (params->policy == UVM_TEST_PAGE_PREFETCH_POLICY_ENABLE)
        va_space->test_page_prefetch_enabled = true;
    else
        va_space->test_page_prefetch_enabled = false;

    uvm_va_space_up_write(va_space);

    return NV_OK;
}

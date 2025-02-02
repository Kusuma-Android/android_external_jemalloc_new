#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/sz.h"

/******************************************************************************/
/* Data. */

static base_t *b0;

/******************************************************************************/

static void *
base_map(tsdn_t *tsdn, ehooks_t *ehooks, unsigned ind, size_t size) {
	void *addr;
	bool zero = true;
	bool commit = true;

	/* Use huge page sizes and alignment regardless of opt_metadata_thp. */
	assert(size == HUGEPAGE_CEILING(size));
	size_t alignment = HUGEPAGE;
	if (ehooks_are_default(ehooks)) {
		addr = extent_alloc_mmap(NULL, size, alignment, &zero, &commit);
	} else {
		addr = ehooks_alloc(tsdn, ehooks, NULL, size, alignment, &zero,
		    &commit);
	}

	return addr;
}

static void
base_unmap(tsdn_t *tsdn, ehooks_t *ehooks, unsigned ind, void *addr,
    size_t size) {
	/*
	 * Cascade through dalloc, decommit, purge_forced, and purge_lazy,
	 * stopping at first success.  This cascade is performed for consistency
	 * with the cascade in extent_dalloc_wrapper() because an application's
	 * custom hooks may not support e.g. dalloc.  This function is only ever
	 * called as a side effect of arena destruction, so although it might
	 * seem pointless to do anything besides dalloc here, the application
	 * may in fact want the end state of all associated virtual memory to be
	 * in some consistent-but-allocated state.
	 */
	if (ehooks_are_default(ehooks)) {
		if (!extent_dalloc_mmap(addr, size)) {
			goto label_done;
		}
		if (!pages_decommit(addr, size)) {
			goto label_done;
		}
		if (!pages_purge_forced(addr, size)) {
			goto label_done;
		}
		if (!pages_purge_lazy(addr, size)) {
			goto label_done;
		}
		/* Nothing worked.  This should never happen. */
		not_reached();
	} else {
		if (!ehooks_dalloc(tsdn, ehooks, addr, size, true)) {
			goto label_done;
		}
		if (!ehooks_decommit(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		if (!ehooks_purge_forced(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		if (!ehooks_purge_lazy(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		/* Nothing worked.  That's the application's problem. */
	}
label_done:
	return;
}

static void
base_edata_init(size_t *extent_sn_next, edata_t *edata, void *addr,
    size_t size) {
	size_t sn;

	sn = *extent_sn_next;
	(*extent_sn_next)++;

	edata_binit(edata, addr, size, sn);
}

static size_t
base_get_num_blocks(base_t *base, bool with_new_block) {
	base_block_t *b = base->blocks;
	assert(b != NULL);

	size_t n_blocks = with_new_block ? 2 : 1;
	while (b->next != NULL) {
		n_blocks++;
		b = b->next;
	}

	return n_blocks;
}

static void *
base_extent_bump_alloc_helper(edata_t *edata, size_t *gap_size, size_t size,
    size_t alignment) {
	void *ret;

	assert(alignment == ALIGNMENT_CEILING(alignment, QUANTUM));
	assert(size == ALIGNMENT_CEILING(size, alignment));

	*gap_size = ALIGNMENT_CEILING((uintptr_t)edata_addr_get(edata),
	    alignment) - (uintptr_t)edata_addr_get(edata);
	ret = (void *)((uintptr_t)edata_addr_get(edata) + *gap_size);
	assert(edata_bsize_get(edata) >= *gap_size + size);
	edata_binit(edata, (void *)((uintptr_t)edata_addr_get(edata) +
	    *gap_size + size), edata_bsize_get(edata) - *gap_size - size,
	    edata_sn_get(edata));
	return ret;
}

static void
base_extent_bump_alloc_post(base_t *base, edata_t *edata, size_t gap_size,
    void *addr, size_t size) {
	if (edata_bsize_get(edata) > 0) {
		/*
		 * Compute the index for the largest size class that does not
		 * exceed extent's size.
		 */
		szind_t index_floor =
		    sz_size2index(edata_bsize_get(edata) + 1) - 1;
		edata_heap_insert(&base->avail[index_floor], edata);
	}

	if (config_stats) {
		base->allocated += size;
		/*
		 * Add one PAGE to base_resident for every page boundary that is
		 * crossed by the new allocation. Adjust n_thp similarly when
		 * metadata_thp is enabled.
		 */
		base->resident += PAGE_CEILING((uintptr_t)addr + size) -
		    PAGE_CEILING((uintptr_t)addr - gap_size);
		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
	}
}

static void *
base_extent_bump_alloc(base_t *base, edata_t *edata, size_t size,
    size_t alignment) {
	void *ret;
	size_t gap_size;

	ret = base_extent_bump_alloc_helper(edata, &gap_size, size, alignment);
	base_extent_bump_alloc_post(base, edata, gap_size, ret, size);
	return ret;
}

/*
 * Allocate a block of virtual memory that is large enough to start with a
 * base_block_t header, followed by an object of specified size and alignment.
 * On success a pointer to the initialized base_block_t header is returned.
 */
static base_block_t *
base_block_alloc(tsdn_t *tsdn, base_t *base, ehooks_t *ehooks, unsigned ind,
    pszind_t *pind_last, size_t *extent_sn_next, size_t size,
    size_t alignment) {
	alignment = ALIGNMENT_CEILING(alignment, QUANTUM);
	size_t usize = ALIGNMENT_CEILING(size, alignment);
	size_t header_size = sizeof(base_block_t);
	size_t gap_size = ALIGNMENT_CEILING(header_size, alignment) -
	    header_size;
	/*
	 * Create increasingly larger blocks in order to limit the total number
	 * of disjoint virtual memory ranges.  Choose the next size in the page
	 * size class series (skipping size classes that are not a multiple of
	 * HUGEPAGE), or a size large enough to satisfy the requested size and
	 * alignment, whichever is larger.
	 */
	size_t min_block_size = HUGEPAGE_CEILING(sz_psz2u(header_size + gap_size
	    + usize));
	pszind_t pind_next = (*pind_last + 1 < sz_psz2ind(SC_LARGE_MAXCLASS)) ?
	    *pind_last + 1 : *pind_last;
	size_t next_block_size = HUGEPAGE_CEILING(sz_pind2sz(pind_next));
	size_t block_size = (min_block_size > next_block_size) ? min_block_size
	    : next_block_size;
	base_block_t *block = (base_block_t *)base_map(tsdn, ehooks, ind,
	    block_size);
	if (block == NULL) {
		return NULL;
	}

	*pind_last = sz_psz2ind(block_size);
	block->size = block_size;
	block->next = NULL;
	assert(block_size >= header_size);
	base_edata_init(extent_sn_next, &block->edata,
	    (void *)((uintptr_t)block + header_size), block_size - header_size);
	return block;
}

/*
 * Allocate an extent that is at least as large as specified size, with
 * specified alignment.
 */
static edata_t *
base_extent_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment) {
	malloc_mutex_assert_owner(tsdn, &base->mtx);

	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	/*
	 * Drop mutex during base_block_alloc(), because an extent hook will be
	 * called.
	 */
	malloc_mutex_unlock(tsdn, &base->mtx);
	base_block_t *block = base_block_alloc(tsdn, base, ehooks,
	    base_ind_get(base), &base->pind_last, &base->extent_sn_next, size,
	    alignment);
	malloc_mutex_lock(tsdn, &base->mtx);
	if (block == NULL) {
		return NULL;
	}
	block->next = base->blocks;
	base->blocks = block;
	if (config_stats) {
		base->allocated += sizeof(base_block_t);
		base->resident += PAGE_CEILING(sizeof(base_block_t));
		base->mapped += block->size;

		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
	}
	return &block->edata;
}

base_t *
b0get(void) {
	return b0;
}

base_t *
base_new(tsdn_t *tsdn, unsigned ind, const extent_hooks_t *extent_hooks,
    bool metadata_use_hooks) {
	pszind_t pind_last = 0;
	size_t extent_sn_next = 0;

	/*
	 * The base will contain the ehooks eventually, but it itself is
	 * allocated using them.  So we use some stack ehooks to bootstrap its
	 * memory, and then initialize the ehooks within the base_t.
	 */
	ehooks_t fake_ehooks;
	ehooks_init(&fake_ehooks, metadata_use_hooks ?
	    (extent_hooks_t *)extent_hooks :
	    (extent_hooks_t *)&ehooks_default_extent_hooks, ind);

	base_block_t *block = base_block_alloc(tsdn, NULL, &fake_ehooks, ind,
	    &pind_last, &extent_sn_next, sizeof(base_t), QUANTUM);
	if (block == NULL) {
		return NULL;
	}

	size_t gap_size;
	size_t base_alignment = CACHELINE;
	size_t base_size = ALIGNMENT_CEILING(sizeof(base_t), base_alignment);
	base_t *base = (base_t *)base_extent_bump_alloc_helper(&block->edata,
	    &gap_size, base_size, base_alignment);
	ehooks_init(&base->ehooks, (extent_hooks_t *)extent_hooks, ind);
	ehooks_init(&base->ehooks_base, metadata_use_hooks ?
	    (extent_hooks_t *)extent_hooks :
	    (extent_hooks_t *)&ehooks_default_extent_hooks, ind);
	if (malloc_mutex_init(&base->mtx, "base", WITNESS_RANK_BASE,
	    malloc_mutex_rank_exclusive)) {
		base_unmap(tsdn, &fake_ehooks, ind, block, block->size);
		return NULL;
	}
	base->pind_last = pind_last;
	base->extent_sn_next = extent_sn_next;
	base->blocks = block;
	for (szind_t i = 0; i < SC_NSIZES; i++) {
		edata_heap_new(&base->avail[i]);
	}
	if (config_stats) {
		base->allocated = sizeof(base_block_t);
		base->resident = PAGE_CEILING(sizeof(base_block_t));
		base->mapped = block->size;
		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
	}
	base_extent_bump_alloc_post(base, &block->edata, gap_size, base,
	    base_size);

	return base;
}

void
base_delete(tsdn_t *tsdn, base_t *base) {
	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	base_block_t *next = base->blocks;
	do {
		base_block_t *block = next;
		next = block->next;
		base_unmap(tsdn, ehooks, base_ind_get(base), block,
		    block->size);
	} while (next != NULL);
}

ehooks_t *
base_ehooks_get(base_t *base) {
	return &base->ehooks;
}

ehooks_t *
base_ehooks_get_for_metadata(base_t *base) {
	return &base->ehooks_base;
}

extent_hooks_t *
base_extent_hooks_set(base_t *base, extent_hooks_t *extent_hooks) {
	extent_hooks_t *old_extent_hooks =
	    ehooks_get_extent_hooks_ptr(&base->ehooks);
	ehooks_init(&base->ehooks, extent_hooks, ehooks_ind_get(&base->ehooks));
	return old_extent_hooks;
}

static void *
base_alloc_impl(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment,
    size_t *esn) {
	alignment = QUANTUM_CEILING(alignment);
	size_t usize = ALIGNMENT_CEILING(size, alignment);
	size_t asize = usize + alignment - QUANTUM;

	edata_t *edata = NULL;
	malloc_mutex_lock(tsdn, &base->mtx);
	for (szind_t i = sz_size2index(asize); i < SC_NSIZES; i++) {
		edata = edata_heap_remove_first(&base->avail[i]);
		if (edata != NULL) {
			/* Use existing space. */
			break;
		}
	}
	if (edata == NULL) {
		/* Try to allocate more space. */
		edata = base_extent_alloc(tsdn, base, usize, alignment);
	}
	void *ret;
	if (edata == NULL) {
		ret = NULL;
		goto label_return;
	}

	ret = base_extent_bump_alloc(base, edata, usize, alignment);
	if (esn != NULL) {
		*esn = (size_t)edata_sn_get(edata);
	}
label_return:
	malloc_mutex_unlock(tsdn, &base->mtx);
	return ret;
}

/*
 * base_alloc() returns zeroed memory, which is always demand-zeroed for the
 * auto arenas, in order to make multi-page sparse data structures such as radix
 * tree nodes efficient with respect to physical memory usage.  Upon success a
 * pointer to at least size bytes with specified alignment is returned.  Note
 * that size is rounded up to the nearest multiple of alignment to avoid false
 * sharing.
 */
void *
base_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment) {
	return base_alloc_impl(tsdn, base, size, alignment, NULL);
}

edata_t *
base_alloc_edata(tsdn_t *tsdn, base_t *base) {
	size_t esn;
	edata_t *edata = base_alloc_impl(tsdn, base, sizeof(edata_t),
	    EDATA_ALIGNMENT, &esn);
	if (edata == NULL) {
		return NULL;
	}
	edata_esn_set(edata, esn);
	return edata;
}

void
base_stats_get(tsdn_t *tsdn, base_t *base, size_t *allocated, size_t *resident,
    size_t *mapped) {
	cassert(config_stats);

	malloc_mutex_lock(tsdn, &base->mtx);
	assert(base->allocated <= base->resident);
	assert(base->resident <= base->mapped);
	*allocated = base->allocated;
	*resident = base->resident;
	*mapped = base->mapped;
	malloc_mutex_unlock(tsdn, &base->mtx);
}

void
base_prefork(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_prefork(tsdn, &base->mtx);
}

void
base_postfork_parent(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_postfork_parent(tsdn, &base->mtx);
}

void
base_postfork_child(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_postfork_child(tsdn, &base->mtx);
}

bool
base_boot(tsdn_t *tsdn) {
	b0 = base_new(tsdn, 0, (extent_hooks_t *)&ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	return (b0 == NULL);
}

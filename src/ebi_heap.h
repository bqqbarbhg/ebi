#ifndef EBI_HEAP_H
#define EBI_HEAP_H

#include "ebi_platform.h"

typedef struct ebi_slab ebi_slab;
typedef struct ebi_heap_class ebi_heap_class;
typedef struct ebi_heap_class_cache ebi_heap_class_cache;
typedef struct ebi_heap_cache ebi_heap_cache;

struct ebi_heap_class {
	uint16_t max_size;
	uint16_t slab_offset, slab_count;
};

#define EBI_HEAP_MAX_CLASS_SIZE 2048

// Generated by misc/make_heap_sizes.py
#define EBI_HEAP_CLASSES 22
#define EBI_HEAP_SLAB_MASKS 10000000 // TODO

extern const ebi_heap_class ebi_heap_classes[EBI_HEAP_CLASSES];
extern const uint8_t ebi_heap_size_to_class[128];

struct ebi_slab {
	ebi_slab *next;
	char align[16 - sizeof(ebi_slab*)];
	uint32_t mask[8];
	uint32_t num_masks;
	uint32_t pad[3];
	char data[EBI_FLEXIBLE_ARRAY];
};

struct ebi_heap_class_cache {
	ebi_slab *slab;
	uint32_t offset;
	uint32_t ix;
};

struct ebi_heap_cache {
	ebi_heap_class_cache classes[EBI_HEAP_CLASSES];
	uint32_t masks[EBI_HEAP_SLAB_MASKS];
};

size_t ebi_slab_get_free(const ebi_slab *slab, uint8_t *dst);

uint32_t ebi_heap_alloc_slow(ebi_heap_cache *c, uint32_t cls, uint32_t ix);
void *ebi_heap_alloc_big(ebi_heap_cache *c, size_t size);

static ebi_forceinline void *
ebi_heap_alloc(ebi_heap_cache *c, size_t size, uint32_t *header_offset)
{
	if (size <= EBI_HEAP_MAX_CLASS_SIZE) {
		const uint32_t cls = ebi_heap_size_to_class[(size - 1) / 16];
		const uint32_t stride = ebi_heap_classes[cls].max_size;
		ebi_heap_class_cache *cc = &c->classes[cls];
		ebi_slab *slab = cc->slab;
		const uint32_t ix = cc->ix;
		uint32_t offset = cc->offset;
		const uint32_t mask = c->masks[ix];
		const uint32_t next = ebi_bsf32(mask);
		const uint32_t next_mask = mask & (mask - 1);

		cc->offset = next * stride;
		cc->ix = ix + (next_mask == 0);
		c->masks[ix] = next_mask;

		if ((mask | ix) == 0) {
			offset = ebi_heap_alloc_slow(c, cls, ix);
			slab = cc->slab;
		}

		return slab->data + offset;
	} else {
		void *ptr = ebi_heap_alloc_big(c, size);
		*header_offset = 0;
		return ptr;
	}
}

#endif
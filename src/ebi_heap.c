#include "ebi_heap.h"
#include "ebi_intrin.h"

// Generated by misc/make_heap_sizes.py
const ebi_heap_class ebi_heap_classes[EBI_HEAP_CLASSES] = {
        {   16,    0, 128 }, {   32,  128, 128 }, {   48,  256, 128 },
        {   64,  384, 128 }, {   80,  512, 128 }, {   96,  640, 128 },
        {  112,  768, 128 }, {  128,  896, 127 }, {  160, 1023, 102 },
        {  192, 1125,  85 }, {  224, 1210,  72 }, {  256, 1282,  63 },
        {  320, 1345,  51 }, {  384, 1396,  42 }, {  448, 1438,  36 },
        {  512, 1474,  31 }, {  768, 1505,  21 }, { 1024, 1526,  15 },
        { 1280, 1541,  12 }, { 1536, 1553,  10 }, { 1792, 1563,   9 },
        { 2048, 1572,   7 },
};

const uint8_t ebi_heap_size_to_class[128] = {
         0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  9,  9, 10, 10, 11, 11,
        12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
        18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
        21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};

size_t ebi_slab_get_free(ebi_slab *slab, uint8_t *dst)
{
	uint8_t *d = dst;

	uint32_t masks[8];
	for (uint32_t i = 0; i < slab->num_masks; i++) {
		masks[i] = ebi_atomic_xhg32(&slab->mask[i], 0);
	}

	for (uint32_t i = 0; i < slab->num_masks; i++) {
		uint32_t mask = masks[i];

		// Process masks one nibble (4 bits) at a time. We can use SWAR to
		// compute the number of bits set in the lowest N bits per nibble
		// in parllel to `nb_popN`. Using these counts we can do branchless
		// compaction without having a dependency chain between nibbles.

		const uint32_t nb_lsb = 0x11111111;
		uint32_t nb_pop1 = ((mask >> 0) & nb_lsb);
		uint32_t nb_pop2 = nb_pop1 + ((mask >> 1) & nb_lsb);
		uint32_t nb_pop3 = nb_pop2 + ((mask >> 2) & nb_lsb);
		uint32_t nb_pop4 = nb_pop3 + ((mask >> 3) & nb_lsb);

		uint32_t offset = i * 32, end = offset + 32;
		while (offset != end) {
			d[0]       = (uint8_t)(offset + 0);
			d[nb_pop1] = (uint8_t)(offset + 1);
			d[nb_pop2] = (uint8_t)(offset + 2);
			d[nb_pop3] = (uint8_t)(offset + 3);
			d += nb_pop4;

			nb_pop1 >>= 4;
			nb_pop2 >>= 4;
			nb_pop3 >>= 4;
			nb_pop4 >>= 4;
			offset += 4;
		}
	}

	return (size_t)(d - dst);
}

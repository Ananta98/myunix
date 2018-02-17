#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <console.h>
#include <pmm.h>
#include <string.h>

uint32_t *block_map;
uint32_t block_map_size;
static uint32_t block_map_last;

/* mark block block as used */
inline void pmm_set_block(uintptr_t block) {
	block_map[block / 32] |= (1 << (block % 32));
}

/* mark block as available  */
inline void pmm_unset_block(uintptr_t block) {
	block_map[block / 32] &= ~(1 << (block % 32));
}

/* test if block is used  */
inline bool pmm_test_block(uintptr_t block) {
	return (block_map[block / 32] & (1 << (block % 32)));
}

inline uint32_t pmm_find_first_free() {
	for (uint32_t i = 0; i < block_map_size; i++) {
		if (block_map[i] != 0xFFFFFFFF) {
			for (uint8_t j = 0; j < 32; j++) {
				uint32_t bit = (1 << j);
				if ( ! (block_map[i] & bit)) {
					return i * 32 + j;
				}
			}
		}
	}

	return 0;
}

inline uint32_t pmm_find_first_free_region(size_t size) {
	assert(size != 0);

	if (size == 0) {
		return 0;
	}

	if (size == 1) {
		return pmm_find_first_free();
	}

	for (uint32_t i = 0; i < block_map_size; i++) {
		if (block_map[i] != 0xFFFFFFFF) {
			for (uint8_t j = 0; j < 32; j++) {
				uint32_t bit = (1 << j);
				if ( ! (block_map[i] & bit)) {
					uint32_t start = i*32+j;
					uint32_t len = 1;
					while (len < size) {
						// TODO: optimize 0x00000000 case
						if (pmm_test_block(start + len)) {
							// block used
							break;
						} else {
							len++;
						}
					}
					if (len == size) {
						return start;
					} else {
						i = i + len / 32;
						j = j + len;
					}
				}
			}
		}
	}

	return 0;
}

void *pmm_alloc_blocks(size_t size) {
	assert(size != 0);
	uint32_t block = pmm_find_first_free_region(size);
	if (block == 0) {
		return NULL;
	}

	for (size_t i = 0; i < size; i++) {
		pmm_set_block(block + i);
	}
	return (void *)(block * BLOCK_SIZE);
}

void pmm_free_blocks(void *p, size_t size) {
	assert(size != 0);

	uint32_t block = ((uint32_t)p) / BLOCK_SIZE;

	for (size_t i = 0; i < size; i++) {
		pmm_unset_block(block + i);
	}
}

uint32_t pmm_count_free_blocks() {
	uint32_t count = 0;
	for (uint32_t i = 0; i <= block_map_size; i++) {
		if (! pmm_test_block(i)) {
			count++;
		}
	}
	return count;
}

void pmm_init(void *mem_map, size_t mem_size) {
	block_map_size = mem_size / BLOCK_SIZE;
	block_map = (uint32_t *)mem_map;
	block_map_last = 0;
	printf("pmm_init at 0x%x with size 0x%x\n", (uint32_t)block_map, (uint32_t)block_map_size);

	memset(block_map, 0xFF, block_map_size);
}

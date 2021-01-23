#ifndef EBI_INTRIN_H
#define EBI_INTRIN_H

#include "ebi_platform.h"

#if EBI_CC_MSC

#include <intrin0.h>

static ebi_forceinline uint32_t ebi_atomic_or32(uint32_t *dst, uint32_t value) {
	return (uint32_t)_InterlockedOr((volatile long*)dst, (long)value);
}

static ebi_forceinline uint32_t ebi_atomic_xhg32(uint32_t *dst, uint32_t value) {
	return (uint32_t)_InterlockedExchange((volatile long*)dst, (long)value);
}

static ebi_forceinline uint32_t ebi_atomic_dcas(uintptr_t *dst, uintptr_t *cmp, uintptr_t lo, uintptr_t hi)
{
#if defined(_M_X64)
	return (bool)_InterlockedCompareExchange128((volatile long long*)dst,
		(long long)hi, (long long) lo, (long long*)cmp);
#else
	long long r = (long long)cmp[1] << 32 | (long long)cmp[0];
	long long v = _InterlockedCompareExchange64((volatile long long*)dst,
		(long long)hi << 32 | (long long)lo, r);
	cmp[0] = (uintptr_t)r;
	cmp[1] = (uintptr_t)(r >> 32);
	return r == v;
#endif
}

#endif

#endif

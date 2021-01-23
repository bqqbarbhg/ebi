#ifndef EBI_PLATFORM_H
#define EBI_PLATFORM_H

#include <stdint.h>

// Compiler and OS abstraction

// -- Feature detection

#define EBI_CC_MSC 0   // Microsoft C
#define EBI_CC_GCC 0   // GCC
#define EBI_CC_CLANG 0 // Clang
#define EBI_CC_GNU 0   // GNU extensions (GCC / Clang)

#define EBI_OS_WIN32 0  // Windows

#if defined(_MSC_VER)
	#undef EBI_CC_MSC
	#define EBI_CC_MSC 1
#elif defined(__clang__)
	#undef EBI_CC_CLANG
	#define EBI_CC_CLANG 1
#else
	#undef EBI_CC_GCC
	#define EBI_CC_GCC 1
#endif

#if EBI_CC_GCC || EBI_CC_CLANG
	#undef EBI_CC_GNU
	#define EBI_CC_GNU 1
#endif

#if defined(_WIN32)
	#undef EBI_OS_WIN32
	#define EBI_OS_WIN32 1
#endif

#ifndef EBI_DEBUG
	#if (EBI_CC_MSC && defined(_DEBUG)) || (!EBI_CC_MSC && !defined(NDEBUG))
		#define EBI_DEBUG 1
	#else
		#define EBI_DEBUG 0
	#endif
#endif

// -- Language extensions

#if EBI_CC_MSC
	#define ebi_forceinline __forceinline inline
	#define ebi_noinline __declspec(noinline)
#elif EBI_CC_GNU
	#define ebi_forceinline __attribute__((always_inline)) inline
	#define ebi_noinline __attribute__((noinline))
#else
	#define ebi_forceinline
	#define ebi_noinline
#endif

#if EBI_CC_MSC
	#define EBI_FLEXIBLE_ARRAY 0
#elif EBI_CC_GNU
	#define EBI_FLEXIBLE_ARRAY 
#else
	#define EBI_FLEXIBLE_ARRAY 1
#endif

// -- Debugging

#if EBI_CC_MSC
	#define ebi_debugbreak() __debugbreak()
#elif EBI_CC_GNU
	#define ebi_debugbreak() __builtin_trap()
#else
	#define ebi_debugbreak() (void)0
#endif

#ifndef ebi_assert
#if EBI_DEBUG
	#if EBI_CC_MSC || EBI_CC_GNU
		#define ebi_assert(cond) do { if (!(cond)) ebi_debugbreak(); } while (0)
	#else
		#include <assert.h>
		#define ebi_assert(cond) assert(cond)
	#endif
#else
	#define ebi_assert(cond) (void)0
#endif
#endif

#endif


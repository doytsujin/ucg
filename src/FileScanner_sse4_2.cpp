/*
 * Copyright 2015-2016 Gary R. Van Sickle (grvs@users.sourceforge.net).
 *
 * This file is part of UniversalCodeGrep.
 *
 * UniversalCodeGrep is free software: you can redistribute it and/or modify it under the
 * terms of version 3 of the GNU General Public License as published by the Free
 * Software Foundation.
 *
 * UniversalCodeGrep is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * UniversalCodeGrep.  If not, see <http://www.gnu.org/licenses/>.
 */

/** @file */

#include <config.h>

#include "FileScanner.h"

#include <libext/multiversioning.hpp>
#include <libext/hints.hpp>

#include <cstdint>   // For uintptr_t.
#include <immintrin.h>


// Declaration here only so we can apply gcc attributes.
inline uint8_t popcount16(uint16_t bits) noexcept __attribute__((const /* Doesn't access globals, has no side-effects.*/,
		artificial /*Should appear in debug info even after being inlined.*/));

#if defined(__POPCNT__) && __POPCNT__==1

/**
 * For systems that support the POPCNT instruction, we can use it through the gcc/clang builtin __builtin_popcount().
 * It inlines nicely into the POPCNT instruction.
 *
 * @param bits
 * @return
 */
inline uint8_t popcount16(uint16_t bits) noexcept
{
	return __builtin_popcount(bits);
}


#else

/**
 * Count the number of bits set in #bits using the Brian Kernighan method (https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetKernighan).
 * Iterates once per set bit, i.e. a maximum of 16 times.
 *
 * @note On systems which do not support POPCNT, we can't use the __builtin_popcount() here.  It expands into a function call
 *       to a generic implementation which is much too slow for our needs here.
 *
 * @param bits  The 16-bit value to count the set bits of.
 * @return The number of bits set in #bits.
 */
inline uint8_t popcount16(uint16_t bits) noexcept
{
	uint8_t num_set_bits { 0 };

	for(; bits; ++num_set_bits)
	{
		bits &= bits-1;
	}

	return num_set_bits;
}

#endif

static constexpr size_t f_alignment { alignof(__m128i) };
static constexpr uintptr_t f_alignment_mask { f_alignment-1 };
static_assert(f_alignment == 16, "alignof(__m128i) should be 16, but isn't");

//__attribute__((target("sse4.2")))
size_t MULTIVERSION(FileScanner::CountLinesSinceLastMatch)(const char * __restrict__ prev_lineno_search_end,
		const char * __restrict__ start_of_current_match) noexcept
{
	size_t num_lines_since_last_match = 0;

	const char * last_ptr = prev_lineno_search_end;

	// Calculate the total number of chars we need to search for '\n's in.
	size_t len = start_of_current_match-prev_lineno_search_end;

	// The character we're looking for, broadcast to all 16 bytes of the looking_for xmm register.
	const __m128i looking_for = _mm_set1_epi8('\n');

	//
	// PROLOGUE
	//

	// Check if we need to handle an unaligned start address.
	if(reinterpret_cast<uintptr_t>(last_ptr) & f_alignment_mask)
	{
		// We do.  Determine how many unaligned prologue bytes we have.
		// These are the bytes starting at last_ptr up to but not including the byte at the first aligned address.
		const size_t num_unaligned_prologue_bytes = f_alignment - (reinterpret_cast<uintptr_t>(last_ptr) & f_alignment_mask);

		// Check if we can use a single unaligned load to search the unaligned starting bytes.
		if(len >= f_alignment)
		{
			// We can, the read won't go past the end of the buffer.

			// Load the first unaligned 16 bytes of the passed string.  Note that we won't actually use
			// all of them in the compare; this is just to get the unaligned portion of the buffer out of the way.
			__m128i substr = _mm_loadu_si128((const __m128i*)last_ptr);

			// Do the match.
			__m128i match_mask = _mm_cmpestrm(substr, num_unaligned_prologue_bytes, looking_for, 16, _SIDD_SBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_BIT_MASK);

			// Get the bottom 32 bits of the match results.  Bits 0-15 tell us if a match happened in the corresponding byte.
			// SSE2, should result in "movd %xxmN, %r32".
			uint32_t match_bitmask = _mm_cvtsi128_si32(match_mask);

			// The above should never result in more than the bottom 16 bits of match_bitmask being set.
			// Hint this to the compiler.  This prevents gcc from adding an unnecessary movzwl %r9w,%r9d before the popcount16() call.
			assume(match_bitmask <= 0xFFFF);

			// Count the bits.
			num_lines_since_last_match += popcount16(match_bitmask);

			// Adjust for the next portion of the counting.
			// Remember, we only searched num_bytes_to_search bytes, not necessarily all 16 bytes we read in.
			last_ptr += num_unaligned_prologue_bytes;
			len -= num_unaligned_prologue_bytes;
		}
		else
		{
			// There aren't 16 bytes to load.  Check the unaligned bytes the slow way.
			while((len > 0) && (reinterpret_cast<uintptr_t>(last_ptr) & f_alignment_mask))
			{
				if(*last_ptr == '\n')
				{
					++num_lines_since_last_match;
				}

				++last_ptr;
				--len;
			}
		}
	}

	if(len == 0)
	{
		// The string didn't have > sizeof(__m128i) chars in it.
		return num_lines_since_last_match;
	}

	/// @note Whatever hints we try to give the compiler here regarding the alignment of last_ptr (including creating a const __m128i *)
	/// make no difference to the generated assembly.  Which seems to be OK, I don't see any problems with what gets generated.

	//
	// MAIN LOOP
	// Process the buffer in aligned, sizeof(__m128i) chunks.
	//
	while(last_ptr < (start_of_current_match-15))
	{
		__m128i substr = _mm_load_si128((const __m128i*)last_ptr);
		__m128i match_mask = _mm_cmpestrm(substr, 16, looking_for, 16, _SIDD_SBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_BIT_MASK);

		uint32_t match_bitmask = _mm_cvtsi128_si32(match_mask);

		// The above should never result in more than the bottom 16 bits of match_bitmask being set.
		// Hint this to the compiler.  This prevents gcc from adding an unnecessary movzwl %r9w,%r9d before the popcount16() call.
		assume(match_bitmask <= 0xFFFF);

		num_lines_since_last_match += popcount16(match_bitmask);
		last_ptr += 16;
		len -= 16;
	}

	// Hint to gcc that these vars have these properties at this point.
	// This does prevent at least gcc from exploding the loop below into a huge amount
	// of sse code.
	assume(len < 16);

	//
	// EPILOGUE
	// Take care of any left over bytes.  They will start on an aligned boundary (last_ptr will be aligned),
	// but there may be anywhere from 0 to 15 bytes.
	//
	while(len > 0)
	{
		if(*last_ptr == '\n')
		{
			++num_lines_since_last_match;
		}

		++last_ptr;
		--len;
	}

	return num_lines_since_last_match;
}



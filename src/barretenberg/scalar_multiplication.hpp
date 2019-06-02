#pragma once

#include "stddef.h"
#include "malloc.h"
#include "memory.h"

#include "g1.hpp"
#include "fr.hpp"
#include "wnaf.hpp"
#include "assert.hpp"

namespace scalar_multiplication
{
constexpr size_t BUCKET_BITS = 26;
constexpr size_t PIPPENGER_ROUNDS = 5;

struct wnaf_runtime_state
{
    uint64_t current_sign;
    uint64_t next_sign;
    size_t current_idx;
    size_t next_idx;
    size_t bits_per_wnaf;
    uint64_t mask;
    uint32_t* wnaf_iterator;
    uint32_t* wnaf_table;
};

struct multiplication_runtime_state
{
    size_t num_points;
    size_t num_rounds;
    size_t num_buckets;

    g1::affine_element *points;
    g1::element *buckets;

    g1::affine_element addition_temporary;
    g1::element accumulator;
    g1::element running_sum;
};

inline void compute_next_bucket_index(wnaf_runtime_state &state)
{
    uint32_t wnaf_entry = *state.wnaf_iterator;
    state.next_sign = (uint64_t)(wnaf_entry >> state.bits_per_wnaf) & 1; // 0 - sign_bit;
    uint32_t sign_mask = 0 - state.next_sign;
    state.next_idx = (((wnaf_entry & ~sign_mask) | (~wnaf_entry & sign_mask)) & state.mask) >> 1;
}

inline void generate_pippenger_point_table(g1::affine_element *points, g1::affine_element *table, size_t num_points)
{
    // iterate backwards, so that `points` and `table` can point to the same memory location
    for (size_t i = num_points - 1; i < num_points; --i)
    {
        g1::copy(&points[i], &table[i * 2]);
        fq::mul_beta(points[i].x, table[i * 2 + 1].x);
        fq::neg(points[i].y, table[i * 2 + 1].y);
    }
}

inline g1::element pippenger(uint64_t *scalars, g1::affine_element *points, size_t num_initial_points, size_t bits_per_bucket) noexcept
{
    multiplication_runtime_state state;
    state.num_points = num_initial_points + num_initial_points;
    state.num_rounds = WNAF_SIZE(bits_per_bucket + 1);
    state.num_buckets = (1 << bits_per_bucket);

    wnaf_runtime_state wnaf_state;

    state.buckets = (g1::element *)aligned_alloc(32, sizeof(g1::element) * state.num_buckets);
    for (size_t i = 0; i < state.num_buckets; ++i)
    {
        g1::set_infinity(state.buckets[i]);
    }

    // allocate space for wnaf table. We need 1 extra entry because our pointer iterator will overflow by 1 in the main loop
    wnaf_state.wnaf_table = (uint32_t *)malloc(sizeof(uint32_t) * state.num_rounds * state.num_points + 1);
    bool *skew_table = (bool *)malloc(sizeof(bool) * state.num_points);

    for (size_t i = 0; i < num_initial_points; ++i)
    {
        if (i % 1000000 == 0)
        {
            printf("processing point %d\n", (int)i);
        }
        fr::split_into_endomorphism_scalars(&scalars[i * 4], &scalars[i * 4], &scalars[i * 4 + 2]);
        wnaf::fixed_wnaf(&scalars[i * 4], &wnaf_state.wnaf_table[2 * i], skew_table[2 * i], state.num_points, bits_per_bucket + 1);
        wnaf::fixed_wnaf(&scalars[i * 4 + 2], &wnaf_state.wnaf_table[2 * i + 1], skew_table[2 * i + 1], state.num_points, bits_per_bucket + 1);
    }


    g1::set_infinity(state.accumulator);

    wnaf_state.bits_per_wnaf = bits_per_bucket + 1;
    wnaf_state.mask = (1 << (wnaf_state.bits_per_wnaf)) - 1;
    wnaf_state.wnaf_iterator = wnaf_state.wnaf_table;
    compute_next_bucket_index(wnaf_state);
    ++wnaf_state.wnaf_iterator;

    for (size_t i = 0; i < state.num_rounds; ++i)
    {
        printf("processing pippenger round %d\n", (int)i);
        for (size_t j = 0; j < state.num_points; ++j)
        {
            // handle 0 as special case
            if (i == (state.num_rounds - 1))
            {
                bool skew = skew_table[j];
                if (skew)
                {
                    g1::mixed_sub(state.buckets[0], points[j], state.buckets[0]);
                }
            }
            wnaf_state.current_idx = wnaf_state.next_idx;
            wnaf_state.current_sign = wnaf_state.next_sign;
            // compute the bucket index one step ahead of our current point, so that
            // we can issue a prefetch instruction and cache the bucket
            compute_next_bucket_index(wnaf_state);
            __builtin_prefetch(&state.buckets[wnaf_state.next_idx]);
            ++wnaf_state.wnaf_iterator;
            g1::conditional_negate_affine(&points[j], &state.addition_temporary, wnaf_state.current_sign);
            // assign_point_to_temp(&points[j], &temp, wnaf_state.current_sign);
            g1::mixed_add(state.buckets[wnaf_state.current_idx], state.addition_temporary, state.buckets[wnaf_state.current_idx]);
        }

        if (i > 0)
        {
            // we want to perform *bits_per_wnaf* number of doublings (i.e. bits_per_bucket + 1)
            // perform all but 1 of the point doubling ops here, we do the last one after accumulating buckets
            for (size_t j = 0; j < bits_per_bucket; ++j)
            {
                g1::dbl(state.accumulator, state.accumulator);
            }
        }
        g1::set_infinity(state.running_sum);
        for (int j = state.num_buckets - 1; j > 0; --j)
        {
            g1::add(state.running_sum, state.buckets[(size_t)j], state.running_sum);
            g1::add(state.accumulator, state.running_sum, state.accumulator);
            g1::set_infinity(state.buckets[(size_t)j]);
        }
        g1::add(state.running_sum, state.buckets[0], state.running_sum);
        g1::dbl(state.accumulator, state.accumulator);
        g1::add(state.accumulator, state.running_sum, state.accumulator);
        g1::set_infinity(state.buckets[0]);
    }

    printf("finished pippenger, freeing memory\n");
    free(wnaf_state.wnaf_table);
    free(state.buckets);
    free(skew_table);

    return state.accumulator;
}
} // namespace scalar_multiplication
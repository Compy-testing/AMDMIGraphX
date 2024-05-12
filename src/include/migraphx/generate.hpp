/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MIGRAPHX_GUARD_MIGRAPHLIB_GENERATE_HPP
#define MIGRAPHX_GUARD_MIGRAPHLIB_GENERATE_HPP

#include <migraphx/argument.hpp>
#include <migraphx/literal.hpp>
#include <migraphx/type_traits.hpp>
#include <migraphx/config.hpp>
#include <random>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

template <class T, MIGRAPHX_REQUIRES(is_floating_point<T>{})>
constexpr T normalize(unsigned long z)
{
    if(z == 0)
        return T(0);
    const auto max     = 32;
    const double range = max / 2; // NOLINT
    double result      = double(z % max) / range;
    result -= 1;
    return T(result);
}

template <class T, MIGRAPHX_REQUIRES(is_signed<T>{} and not is_floating_point<T>{})>
constexpr T normalize(unsigned long z)
{
    const auto max      = 1ULL << (sizeof(T) * 5);
    const auto half_max = max / 2;
    return half_max - (z % max);
}

template <class T,
          MIGRAPHX_REQUIRES(not is_signed<T>{} and std::is_integral<T>{} and
                            not std::is_same<T, bool>{})>
constexpr T normalize(unsigned long z)
{
    const auto max = 1ULL << (sizeof(T) * 5);
    return z % max;
}

template <class T, MIGRAPHX_REQUIRES(std::is_same<T, bool>{})>
constexpr bool normalize(unsigned long z)
{
    return static_cast<bool>(z % 2);
}

struct xorshf96_engine
{
    unsigned long x = 123456789;
    unsigned long y = 362436069;
    unsigned long z;

    xorshf96_engine(unsigned long seed = 0) : z(521288629ULL ^ seed) {}

    constexpr unsigned long operator()() noexcept
    {
        x ^= x << 16U;
        x ^= x >> 5U;
        x ^= x << 1U;

        unsigned long t = x;
        x               = y;
        y               = z;
        z               = t ^ x ^ y;

        return z;
    }

    using result_type = unsigned long;

    static constexpr unsigned long max() { return std::numeric_limits<unsigned long>::max(); }

    static constexpr unsigned long min() { return std::numeric_limits<unsigned long>::min(); }
};

template <class T>
struct normal_generator
{
    static std::normal_distribution<> make_distribution()
    {
        if constexpr(std::is_same<T, bool>{})
            return std::normal_distribution<>{0, 1};
        if constexpr(std::is_integral<T>{})
        {
            double mid = std::numeric_limits<T>::max() / 4;
            return std::normal_distribution<>{std::is_signed<T>{} ? 0 : mid, mid};
        }
        return std::normal_distribution<>{0, 1};
    }
    xorshf96_engine engine;
    std::normal_distribution<> dist;
    normal_generator(unsigned long seed = 0) : engine(seed), dist{make_distribution()} {}

    T operator()() noexcept
    {
        auto result = dist(engine);
        if constexpr(std::is_same<T, bool>{})
            return result > 0;
        if constexpr(std::is_integral<T>{})
            return T(result);
        return T(result);
        // double bits = std::pow(2, std::numeric_limits<T>::digits);
        // std::int64_t i = std::pow(2, std::numeric_limits<T>::digits) * result;
    }
};

template <class T>
struct xorshf96_generator
{
    unsigned long x = 123456789;
    unsigned long y = 362436069;
    unsigned long z;

    xorshf96_generator(unsigned long seed = 0) : z(521288629ULL ^ seed) {}

    constexpr T operator()() noexcept
    {
        x ^= x << 16U;
        x ^= x >> 5U;
        x ^= x << 1U;

        unsigned long t = x;
        x               = y;
        y               = z;
        z               = t ^ x ^ y;

        return normalize<T>(z);
    }
};

template <class T>
struct xorshift_generator
{
    unsigned long x;

    xorshift_generator(unsigned long seed = 0) : x(521288629ULL ^ seed) {}

    constexpr T operator()() noexcept
    {
        x ^= x >> 12U;
        x ^= x << 25U;
        x ^= x >> 27U;
        return normalize<T>(x * 0x2545F4914F6CDD1D);
    }
};

template <class T>
auto generate_tensor_data(const migraphx::shape& s, unsigned long seed = 0)
{
    auto result = make_shared_array<T>(s.element_space());
    std::generate(result.get(), result.get() + s.element_space(), normal_generator<T>{seed});
    return result;
}

template <class T>
auto fill_tensor_data(const migraphx::shape& s, double value = 0)
{
    auto result = make_shared_array<T>(s.element_space());
    std::generate(result.get(), result.get() + s.element_space(), [=] { return value; });
    return result;
}

MIGRAPHX_EXPORT argument fill_argument(shape s, double value = 0);

MIGRAPHX_EXPORT argument generate_argument(shape s, unsigned long seed = 0);

MIGRAPHX_EXPORT literal generate_literal(shape s, unsigned long seed = 0);

MIGRAPHX_EXPORT literal abs(literal l);

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx

#endif

/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2022 Advanced Micro Devices, Inc. All rights reserved.
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
#include <fstream>
#include <filesystem>
#include <migraphx/gpu/compiler.hpp>
#include <migraphx/make_op.hpp>
#include <migraphx/gpu/context.hpp>

#include <migraphx/gpu/compile_hip_code_object.hpp>
#include <migraphx/gpu/compile_hip.hpp>
#include <migraphx/gpu/compile_gen.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/env.hpp>
#include <migraphx/reduce_dims.hpp>
#include <migraphx/stringutils.hpp>
#include <migraphx/module.hpp>
#include <migraphx/env.hpp>
#include <migraphx/file_buffer.hpp>

const std::vector<std::string>&
get_instance(std::size_t i, const std::function<bool(const std::vector<std::string>&)>& pred);

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

namespace gpu {

using namespace migraphx::gpu::gen; // NOLINT

MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_LOG_CK_GEMM);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_CK_TUNING);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_CK_TUNING_VALUE);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_CK_DEBUG);

// NOLINTNEXTLINE
static const char* const ck_gemm_kernel = R"__migraphx__(
#include <args.hpp>
#include <migraphx/kernels/ck_gemm.hpp>
#include <migraphx/kernels/pointwise.hpp>
#include "ck/ck.hpp"
#include "ck/tensor_operation/gpu/device/impl/device_gemm_multiple_d_dl.hpp"

using Row = ck::tensor_layout::gemm::RowMajor;
using Col = ck::tensor_layout::gemm::ColumnMajor;

template <ck::index_t... Is>
using S = ck::Sequence<Is...>;

using PassThrough = ck::tensor_operation::element_wise::PassThrough;

using Empty_Tuple   = ck::Tuple<>;

using GEMM = ck::tensor_operation::device::${instance1}${padding}${instance2};

namespace migraphx {

${preamble}

extern "C" {

__global__ void ${kernel}(${params})
{
    transform_args(make_tensors(), rotate_last())(${args})([](auto... xs) {
        ck_gemm<GEMM, ${blocks_per_batch}>(xs...);
    });
}

}

} // namespace migraphx

)__migraphx__";

static std::size_t int_div_ceil(std::size_t x, std::size_t y) { return (x + y - 1) / y; }

struct instance
{
    std::vector<std::string> params;
    static const std::size_t block_size_index = 15;

    std::size_t int_at(std::size_t i) const { return std::stoull(params[i]); }

    std::size_t get_block_size() const { return int_at(block_size_index); }

    std::size_t get_pb(std::size_t i) const
    {
        assert(i < 4);
        return int_at(block_size_index + 1 + i);
    }

    std::array<std::size_t, 3> get_pad(const std::array<std::size_t, 3>& config) const
    {
        std::array<std::size_t, 3> result{};
        for(auto i : range(config.size()))
        {
            result[i] = int_div_ceil(config[i], get_pb(i)) * get_pb(i) - config[i];
        }
        return result;
    }

    std::size_t get_grid_size(const std::array<std::size_t, 3>& config) const
    {
        return int_div_ceil(config[0], get_pb(0)) * int_div_ceil(config[1], get_pb(1));
    }

    void set_ds_layout(const std::string& s)
    {
        assert(params[2] == "ck::Tuple<>");
        params[2] = s;
    }

    void set_ds_type(const std::string& s)
    {
        assert(params[8] == "ck::Tuple<>");
        params[8] = s;
    }

    void set_ds_op(const std::string& s)
    {
        assert(params[12] == "ck_passthrough");
        params[12] = s;
    }

    void set_gemm(const std::string& s)
    {
        assert(params[13] == "ck::tensor_operation::device::GemmSpecialization::Default");
        params[13] = s;
    }

    std::string str() const { return join_strings(params, ","); }
};

static bool transposed_matrix(const shape& s) { return s.strides().back() != 1; }

template <class F, class Action>
auto action_decorate(F f, Action action)
{
    return [=](auto&&... xs) {
        action();
        f(std::forward<decltype(xs)>(xs)...);
    };
}

using tuning_entry = std::pair<std::vector<shape>, size_t>;
static std::vector<tuning_entry> read_tuning(const std::string& s)
{
    if(not fs::exists(s))
        return {};
    return from_value<std::vector<tuning_entry>>(from_json_string(read_string(s)));
}

static float matrix_distance(const shape& x, const shape& y)
{
    if(x.type() != y.type())
        return std::numeric_limits<float>::max();
    if(transposed_matrix(x) != transposed_matrix(y))
        return std::numeric_limits<float>::max();
    auto sum_squared = std::inner_product(x.lens().rbegin(),
                                          x.lens().rbegin() + 2,
                                          y.lens().rbegin(),
                                          0,
                                          std::plus<>{},
                                          [](auto a, auto b) { return (a - b) * (a - b); });
    return std::sqrt(sum_squared);
}

static std::size_t get_tuning_for(const std::vector<shape>& inputs)
{
    static auto tuning = read_tuning(string_value_of(MIGRAPHX_CK_TUNING{}, ""));
    if(tuning.empty())
        std::cout << "*********** Warning: No CK tuning!" << std::endl;
    auto it = std::find_if(
        tuning.begin(), tuning.end(), [&](const auto& p) { return p.first == inputs; });
    if(it == tuning.end())
    {
        std::cout << "*********** Warning: CK tuning missing for config!" << std::endl;
        std::vector<std::pair<float, std::size_t>> w;
        std::transform(tuning.begin(), tuning.end(), std::back_inserter(w), [&](const auto& p) {
            if(inputs.size() < 3 or p.first.size() < 3)
                MIGRAPHX_THROW("Invalid CK config");
            auto avg_distance = std::inner_product(
                p.first.begin(),
                p.first.begin() + 3,
                inputs.begin(),
                0.0f,
                std::plus<>{},
                [](const auto& x, const auto& y) { return matrix_distance(x, y) / 3.0f; });
            return std::make_pair(avg_distance, p.second);
        });
        std::sort(w.begin(), w.end());
        std::size_t default_value = 4;
        if(not w.empty())
            default_value = w.front().second;
        auto tuning_val = value_of(MIGRAPHX_CK_TUNING_VALUE{}, default_value);
        std::cout << "*********** Warning: CK try tuning: " << tuning_val << std::endl;
        return tuning_val;
    }
    return it->second;
}

struct ck_gemm_compiler : compiler<ck_gemm_compiler>
{
    static std::string get_layout(const shape& s)
    {
        return transposed_matrix(s) ? "ck::tensor_layout::gemm::ColumnMajor"
                                    : "ck::tensor_layout::gemm::RowMajor";
    }

    static std::string get_type(const shape& s)
    {
        if(s.type() == shape::half_type)
            return "ck::half_t";
        return shape::cpp_type(s.type());
    }

    template <class Iterator, class F>
    static std::string ck_tuple(Iterator start, Iterator last, F f)
    {
        std::vector<std::string> s;
        std::transform(start, last, std::back_inserter(s), f);
        return "ck::Tuple<" + join_strings(s, ",") + ">";
    }

    static std::vector<shape> adjust_inputs(std::vector<shape> inputs, bool& swap_inputs)
    {
        swap_inputs  = false;
        auto c_shape = inputs.back();
        if(not transposed_matrix(c_shape))
            return inputs;
        std::vector<int64_t> perm(c_shape.lens().size());
        std::iota(perm.begin(), perm.end(), 0);
        std::swap(perm[perm.size() - 1], perm[perm.size() - 2]);
        std::transform(inputs.begin(), inputs.end(), inputs.begin(), [&](shape s) {
            return reorder_shape(s, perm);
        });
        swap_inputs = true;
        return inputs;
    }

    static std::size_t get_batch_count(const shape& s)
    {
        return std::accumulate(
            s.lens().rbegin() + 2, s.lens().rend(), std::size_t{1}, std::multiplies<std::size_t>());
    }

    static void fold_batch_dims(shape& s)
    {
        auto lens = s.lens();
        if(lens.size() <= 2)
            return;
        auto batch_count = get_batch_count(s);
        auto m1          = lens.at(lens.size() - 2);
        auto m2          = lens.at(lens.size() - 1);
        if(transposed_matrix(s))
            s = shape{s.type(), {m1, m2 * batch_count}};
        else
            s = shape{s.type(), {m1 * batch_count, m2}};
    }

    static void remove_batch_dims(shape& s)
    {
        auto lens = s.lens();
        if(lens.size() <= 2)
            return;
        auto m1 = lens.at(lens.size() - 2);
        auto m2 = lens.at(lens.size() - 1);
        s       = shape{s.type(), {m1, m2}};
    }

    std::vector<std::string> names() const { return {"ck_gemm", "gpu::ck_gemm"}; }

    operation compile_op(context& /* ctx */, const std::vector<shape>& inputs, const value& v) const
    {
        auto a_shape = inputs[0];
        auto b_shape = inputs[1];
        auto c_shape = inputs.back();
        auto transa  = transposed_matrix(a_shape);
        auto transb  = transposed_matrix(b_shape);
        std::string instance_str1;
        std::string instance_str2;
        if (transa and not transb)  
        {
            instance_str1 = "DeviceGemmMultipleD_Dl<    Col,    Row, Empty_Tuple,    Row, int8_t, int8_t, int32_t, Empty_Tuple,  int8_t, PassThrough, PassThrough,  PassThrough,     ";
            instance_str2 = ",   256,   128,   128,    16,  4,       4,      4,      1,       S<8, 2>,       S<8, 2>,      S<2, 1, 4, 4>,      S<8, 1,  32, 1>,  S<0, 3, 1, 2>,  S<0, 3, 1, 2>,       S<1, 1, 4, 1>,      S<0, 3, 1, 2>,       S<1, 1, 4, 4>,      S<2, 1, 4, 4>,      S<8, 1,  32, 1>,  S<0, 3, 1, 2>,  S<0, 3, 1, 2>,       S<1, 1, 4, 1>,      S<0, 3, 1, 2>,       S<1, 1, 4, 4>, S<0, 1, 2, 3, 4, 5>,               5,                  4>";

        }
        else if (transa and transb)
        {
            instance_str1 = "DeviceGemmMultipleD_Dl<    Col,    Col, Empty_Tuple,    Row, int8_t, int8_t, int32_t, Empty_Tuple,  int8_t, PassThrough, PassThrough,  PassThrough,     ";
            instance_str2 = ",   256,   128,   128,    16,  4,      4,       4,      1,       S<8, 2>,       S<8, 2>,      S<2, 1, 4, 4>,      S<8, 1,  32, 1>,  S<0, 3, 1, 2>,  S<0, 3, 1, 2>,       S<1, 1, 4, 1>,      S<0, 3, 1, 2>,       S<1, 1, 4, 4>,      S<8, 1, 1, 4>,      S<2, 1, 128, 1>,  S<1, 2, 0, 3>,  S<1, 2, 0, 3>,       S<4, 1, 1, 4>,      S<1, 2, 0, 3>,       S<1, 1, 1, 4>, S<0, 1, 2, 3, 4, 5>,               5,                  4>";

        }
        else if (not transa and not transb)
        {
            instance_str1 = "DeviceGemmMultipleD_Dl<    Row,    Row, Empty_Tuple,    Row, int8_t, int8_t, int32_t, Empty_Tuple,  int8_t, PassThrough, PassThrough,  PassThrough,     ";
            instance_str2 = ",   256,   128,   128,    16,  4,       4,      4,      1,       S<8, 2>,       S<8, 2>,      S<8, 1, 1, 4>,      S<2, 1, 128, 1>,  S<1, 2, 0, 3>,  S<1, 2, 0, 3>,       S<4, 1, 1, 4>,      S<1, 2, 0, 3>,       S<1, 1, 1, 4>,      S<2, 1, 4, 4>,      S<8, 1,  32, 1>,  S<0, 3, 1, 2>,  S<0, 3, 1, 2>,       S<1, 1, 4, 1>,      S<0, 3, 1, 2>,       S<1, 1, 4, 4>, S<0, 1, 2, 3, 4, 5>,               5,                  4>";

        }
        else
        {
            instance_str1 = "DeviceGemmMultipleD_Dl<    Row,    Col, Empty_Tuple,    Row, int8_t, int8_t, int32_t, Empty_Tuple,  int8_t, PassThrough, PassThrough,  PassThrough,     ";
            instance_str2 = ",   256,   128,   128,    16,  4,       4,      4,      1,       S<8, 2>,       S<8, 2>,      S<8, 1, 1, 4>,      S<2, 1, 128, 1>,  S<1, 2, 0, 3>,  S<1, 2, 0, 3>,       S<4, 1, 1, 4>,      S<1, 2, 0, 3>,       S<1, 1, 1, 4>,      S<8, 1, 1, 4>,      S<2, 1, 128, 1>,  S<1, 2, 0, 3>,  S<1, 2, 0, 3>,       S<4, 1, 1, 4>,      S<1, 2, 0, 3>,       S<1, 1, 1, 4>, S<0, 1, 2, 3, 4, 5>,               5,                  4>";

        }

        auto rank           = a_shape.lens().size();
        auto b_strides      = b_shape.strides();
        bool can_fold_batch = rank >= 3 and b_strides[rank - 3] == 0;

        auto batch_count = get_batch_count(c_shape);
        auto m           = c_shape.lens()[rank - 2];
        m                = can_fold_batch ? m * batch_count : m;
        auto n           = c_shape.lens().back();
        auto k           = a_shape.lens().back();
        std::array<char, 3> keys{'M', 'N', 'K'};
        std::array<std::size_t, 3> config{m, n, k};
        auto tuning_val = v.get("tuning_val", get_tuning_for({a_shape, b_shape, c_shape}));
        auto ip         = instance{get_instance(tuning_val, [&](const auto& x) -> bool {
            return true; /* get_layout(a_shape) == x[0] and get_layout(b_shape) == x[1] and
                    get_layout(c_shape) == x[3] and get_type(a_shape) == x[4] and
                    get_type(b_shape) == x[5] and get_type(c_shape) == x[9]; */
        })};
        assert(inputs.size() < 4 or v.contains("post"));
        if(v.contains("post"))
        {
            ip.set_ds_layout(ck_tuple(inputs.begin() + 2, inputs.end() - 1, &get_layout));
            ip.set_ds_type(ck_tuple(inputs.begin() + 2, inputs.end() - 1, &get_type));
            ip.set_ds_op(v.at("post").to<std::string>());
        }

        auto m_per_block = 128;
        auto n_per_block = 128;
        auto k_per_block = 16;

        auto padding = ip.get_pad(config);
        std::string gemm_type;
        // if (int_div_ceil(m, m_per_block) * m_per_block - m != 0)
        //     gemm_type += "M";
        // if (int_div_ceil(n, n_per_block) * n_per_block - n != 0)
        //     gemm_type += "N";
        // if (int_div_ceil(k, k_per_block) * k_per_block - k != 0)
        //     gemm_type += "K";
        if ((int_div_ceil(m, m_per_block) * m_per_block - m != 0) or (int_div_ceil(n, n_per_block) * n_per_block - n != 0))
            gemm_type = "MNPadding";
        else
            gemm_type = "Default";
        ip.set_gemm("ck::tensor_operation::device::GemmSpecialization::" + gemm_type);
        std::string padding_str = "ck::tensor_operation::device::GemmSpecialization::" + gemm_type;
        std::cout << padding_str << std::endl;
        //std::exit(0);
        auto blocks_per_batch = int_div_ceil(m, 128) * int_div_ceil(n, 128);
        ; // ip.get_grid_size(config);

        hip_compile_options options;
        auto block_size = 256; // ip.get_block_size();
        auto grid_size  = can_fold_batch ? blocks_per_batch : batch_count * blocks_per_batch;
        options.set_launch_params(v, grid_size * block_size, block_size);
        // auto new_inputs = inputs;
        auto new_inputs = inputs;
        // auto out_s = inputs.back();
        // new_inputs.back() = shape{shape::int8_type, out_s.lens(), out_s.strides()};
        options.inputs         = new_inputs;
        options.output         = c_shape;
        options.kernel_name    = v.get("kernel", "ck_gemm_kernel");
        options.virtual_inputs = new_inputs;
        if(can_fold_batch)
        {
            auto vinputs = new_inputs;
            fold_batch_dims(vinputs[0]);
            remove_batch_dims(vinputs[1]);
            std::for_each(vinputs.begin() + 2, vinputs.end(), fold_batch_dims);
            options.virtual_inputs = vinputs;
        }

        if(v.get("check", false) or enabled(MIGRAPHX_CK_DEBUG{}))
            options.params += " -DMIGRAPHX_CK_CHECK=1";

        auto src = interpolate_string(ck_gemm_kernel,
                                      {{"instance1", instance_str1},
                                       {"instance2", instance_str2},
                                       {"padding", padding_str},
                                       {"params", enum_params(inputs.size(), "void * private_p")},
                                       {"args", enum_params(inputs.size(), "private_p")},
                                       {"blocks_per_batch", to_string(blocks_per_batch)},
                                       {"preamble", v.get("preamble", std::string{})},
                                       {"kernel", options.kernel_name}});

        return compile_hip_code_object(src, options);
    }

    compiler_replace compile(context& ctx, instruction_ref ins, const operation& op) const
    {
        auto v      = op.to_value();
        v["kernel"] = "ck_gemm_kernel";
        if(not ins->module_inputs().empty())
        {
            auto* pm      = ins->module_inputs().front();
            v["preamble"] = generate_pointwise(*pm, "post_ck_gemm_function") +
                            "\nMIGRAPHX_LIFT_CLASS(post_ck_gemm, post_ck_gemm_function);";
            v["post"]   = "ck_function_adaptor<post_ck_gemm>";
            v["kernel"] = "ck_gemm_" + generate_name_from_ops(*pm) + "_kernel";
        }

        auto shapes = to_shapes(ins->inputs());
        return action_decorate(replace(compile_op(ctx, shapes, v)), [=] {
            if(enabled(MIGRAPHX_LOG_CK_GEMM{}))
            {
                std::vector<shape> gemm_shapes{shapes[0], shapes[1], shapes.back()};
                std::cout << "ck_gemm: " << to_json_string(to_value(gemm_shapes)) << std::endl;
            }
        });
    }
};

} // namespace gpu
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx

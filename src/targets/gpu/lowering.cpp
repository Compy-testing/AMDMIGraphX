#include <rocblas.h>
#include <migraphx/gpu/lowering.hpp>
#include <migraphx/manage_ptr.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/operators.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/shape_for_each.hpp>
#include <migraphx/gpu/miopen.hpp>
#include <migraphx/gpu/hip.hpp>
#include <migraphx/dfor.hpp>
#include <migraphx/gpu/device/contiguous.hpp>
#include <migraphx/gpu/device/add.hpp>
#include <migraphx/iterator_for.hpp>
#include <migraphx/gpu/rocblas.hpp>
#include <migraphx/gpu/context.hpp>
#include <migraphx/gpu/convolution.hpp>
#include <migraphx/gpu/contiguous.hpp>
#include <migraphx/gpu/relu.hpp>
#include <migraphx/gpu/sigmoid.hpp>
#include <migraphx/gpu/abs.hpp>
#include <migraphx/gpu/leaky_relu.hpp>
#include <migraphx/gpu/elu.hpp>
#include <migraphx/gpu/softmax.hpp>
#include <migraphx/gpu/add.hpp>
#include <migraphx/gpu/sin.hpp>
#include <migraphx/gpu/cos.hpp>
#include <migraphx/gpu/tan.hpp>
#include <migraphx/gpu/sinh.hpp>
#include <migraphx/gpu/cosh.hpp>
#include <migraphx/gpu/tanh.hpp>
#include <migraphx/gpu/asin.hpp>
#include <migraphx/gpu/acos.hpp>
#include <migraphx/gpu/atan.hpp>
#include <migraphx/gpu/mul.hpp>
#include <migraphx/gpu/batchnorm.hpp>
#include <migraphx/gpu/pooling.hpp>
#include <migraphx/gpu/gemm.hpp>
#include <migraphx/gpu/concat.hpp>
#include <utility>
#include <functional>

namespace migraphx {
inline namespace MIGRAPH_INLINE_NS {
namespace gpu {

struct miopen_apply
{
    program* prog = nullptr;
    context ctx{};
    std::unordered_map<std::string, std::function<instruction_ref(instruction_ref)>>
        apply_map{};

    void check_shape(shape x, instruction_ref i)
    {
        assert(x == i->get_shape());
        (void)x;
        (void)i;
    }

    void init()
    {
        add_miopen_simple_op("relu", miopen_relu{}, make_relu);
        add_miopen_simple_op("sigmoid", miopen_sigmoid{}, make_sigmoid);
        add_miopen_simple_op("abs", miopen_abs{}, make_abs);
        add_miopen_simple_op("tanh", miopen_tanh{}, make_tanh);

        add_miopen_extend_op("leaky_relu", miopen_leaky_relu{}, op::leaky_relu{}, make_leaky_relu);
        add_miopen_extend_op("elu", miopen_elu{}, op::elu{}, make_elu);

        add_generic_op("add", hip_add{});
        add_generic_op("sin", hip_sin{});
        add_generic_op("cos", hip_cos{});
        add_generic_op("tan", hip_tan{});
        add_generic_op("sinh", hip_sinh{});
        add_generic_op("cosh", hip_cosh{});
        add_generic_op("asin", hip_asin{});
        add_generic_op("acos", hip_acos{});
        add_generic_op("atan", hip_atan{});
        add_generic_op("mul", hip_mul{});

        add_extend_op("dot", miopen_gemm{}, op::dot{});
        add_extend_op("contiguous", miopen_contiguous{}, op::contiguous{});
        add_extend_op("concat", hip_concat{}, op::concat{});
        add_extend_op("softmax", miopen_softmax{}, op::softmax{});

        add_convolution_op();
        add_pooling_op();
        add_batch_norm_inference_op();
    }

    void apply()
    {
        init();
        for(auto it = prog->begin(); it != prog->end(); it++)
        {
            auto s = it->get_shape();
            if(apply_map.count(it->name()) > 0)
            {
                check_shape(s, apply_map.at(it->name())(it));
            }
        }
    }

    instruction_ref insert_allocation(instruction_ref ins, const shape& s, std::string tag = "")
    {
        if(ins == --prog->end() and tag.empty())
        {
            return prog->add_parameter("output", s);
        }
        else
        {
            auto is     = prog->add_outline(s);
            auto result = prog->insert_instruction(ins, hip_allocate{std::move(tag)}, is);
            return result;
        }
    }

    void add_convolution_op() {
        apply_map.emplace("convolution", [=](instruction_ref ins) {
            auto&& op = any_cast<op::convolution>(ins->get_operator());

            auto conv = miopen_convolution{op, make_conv(op)};
            auto ws   = conv.compile(ctx, ins->get_shape(), ins->inputs());

            auto workspace = insert_allocation(ins, ws, "workspace");
            auto output    = insert_allocation(ins, ins->get_shape());

            return prog->replace_instruction(
                ins, conv, ins->inputs().at(0), ins->inputs().at(1), workspace, output);
        });
    }

    void add_pooling_op() {
        apply_map.emplace("pooling", [=](instruction_ref ins) {
            auto&& op   = any_cast<op::pooling>(ins->get_operator());
            auto pd     = make_pooling(op);
            auto output = insert_allocation(ins, ins->get_shape());

            return prog->replace_instruction(
                ins, miopen_pooling{op, std::move(pd)}, ins->inputs().at(0), output);
        });
    }

    template<class T>
    void add_generic_op(std::string name, T x)
    {
        apply_map.emplace(name, [=](instruction_ref ins) {
            auto output                       = insert_allocation(ins, ins->get_shape());
            std::vector<instruction_ref> refs = ins->inputs();
            refs.push_back(output);

            return prog->replace_instruction(ins, T{}, refs);
        });
        (void)x;
    }

    template<class T, class Op>
    void add_extend_op(std::string name, T x, Op o)
    {
        apply_map.emplace(name, [=](instruction_ref ins) {
            auto&& op                         = any_cast<Op>(ins->get_operator());
            auto output                       = insert_allocation(ins, ins->get_shape());
            std::vector<instruction_ref> refs = ins->inputs();
            refs.push_back(output);

            return prog->replace_instruction(ins, T{op}, refs);
        });
        (void)x;
        (void)o;
    }

    template<class T, class Op, class F>
    void add_miopen_extend_op(std::string name, T x, Op o, F f) {
            apply_map.emplace(name, [=](instruction_ref ins) {
            auto&& op = any_cast<Op>(ins->get_operator());
            auto ad   = f(op.alpha);

            auto output = insert_allocation(ins, ins->get_shape());
            return prog->replace_instruction(
                ins, T{std::move(ad)}, ins->inputs().at(0), output);
        });
        (void)x;
        (void)o;
        (void)f;
    }

    template<class T, class F>
    void add_miopen_simple_op(std::string name, T x, F f) {
            apply_map.emplace(name, [=](instruction_ref ins) {
            auto ad   = f();
            auto output = insert_allocation(ins, ins->get_shape());
            return prog->replace_instruction(
                ins, T{std::move(ad)}, ins->inputs().at(0), output);
        });
        (void)x;
        (void)f;
    }

    void add_batch_norm_inference_op() {
        apply_map.emplace("batch_norm_inference", [=](instruction_ref ins) {
            auto&& op       = any_cast<op::batch_norm_inference>(ins->get_operator());
            auto output     = insert_allocation(ins, ins->get_shape());
            shape old_shape = ins->inputs().at(1)->get_shape();
            std::vector<int64_t> new_shape{1, static_cast<int64_t>(old_shape.elements()), 1, 1};
            auto reshape_op = op::reshape{new_shape};
            std::vector<instruction_ref> reshapes;
            std::transform(ins->inputs().begin() + 1,
                        ins->inputs().end(),
                        std::back_inserter(reshapes),
                        [&](auto i) { return prog->insert_instruction(ins, reshape_op, i); });
            return prog->replace_instruction(ins,
                                            miopen_batch_norm_inference{op},
                                            ins->inputs().at(0),
                                            reshapes[0],
                                            reshapes[1],
                                            reshapes[2],
                                            reshapes[3],
                                            output);
        });
    }
};

void lowering::apply(program& p) const { miopen_apply{&p, ctx}.apply(); }
} // namespace gpu
} // namespace MIGRAPH_INLINE_NS
} // namespace migraphx

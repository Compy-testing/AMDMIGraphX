#include <migraphx/fuse_concat.hpp>
#include <migraphx/pass_manager.hpp>
#include <migraphx/module.hpp>
#include <migraphx/dead_code_elimination.hpp>
#include <migraphx/algorithm.hpp>
#include <migraphx/check_shapes.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/matcher.hpp>
#include <migraphx/make_op.hpp>
#include <migraphx/register_op.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

struct fused_concat
{
    int64_t axis = 0;

    std::string name() const { return "fused_concat"; }

    template <class Self, class F>
    static auto reflect(Self& self, F f)
    {
        return pack(f(self.axis, "axis"));
    }

    shape compute_shape(std::vector<shape> inputs, const std::vector<module_ref>& mods) const
    {
        check_shapes{inputs, *this}.same_ndims();
        if((inputs.size() + 1) == mods.size())
            MIGRAPHX_THROW("FUSED_CONCAT: Missing fused modules");
        auto input_iter = inputs.begin();
        std::vector<shape> concat_inputs;
        for(module_ref mod : range(mods.begin(), mods.end() - 1))
        {
            concat_inputs.push_back(*input_iter);
            input_iter += mod->get_parameter_names().size();
        }
        module_ref post_mod          = mods.back();
        auto type                    = std::prev(post_mod->end())->get_shape().type();
        const auto& first_shape_lens = concat_inputs.front().lens();
        if(not std::all_of(concat_inputs.begin() + 1, concat_inputs.end(), [&](auto s) {
               const auto& lens = s.lens();
               return std::equal(lens.begin(),
                                 lens.begin() + axis,
                                 first_shape_lens.begin(),
                                 first_shape_lens.begin() + axis) and
                      std::equal(lens.begin() + axis + 1,
                                 lens.end(),
                                 first_shape_lens.begin() + axis + 1,
                                 first_shape_lens.end());
           }))
            MIGRAPHX_THROW("FUSED_CONCAT: all input dimensions should match along non-axis: " +
                           std::to_string(axis));

        std::size_t new_dim_axis = transform_accumulate(
            concat_inputs.begin(), concat_inputs.end(), 0, std::plus<>{}, [&](const auto& input) {
                return input.lens()[axis];
            });
        auto new_lens  = concat_inputs.front().lens();
        new_lens[axis] = new_dim_axis;
        return shape::from_permutation(type, new_lens, find_permutation(inputs));
    }
};
MIGRAPHX_REGISTER_OP(fused_concat);

namespace {

static unsigned int counter = 0;
struct find_pointwise_concat_pointwise
{
    auto matcher() const
    {
        auto concat = match::name("concat")(
            match::used_once(),
            match::any_of[match::inputs()](match::name("pointwise")(match::used_once())));
        return match::name("pointwise")(match::any_of[match::inputs()](concat.bind("concat")));
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto ins        = r.result;
        auto concat_ins = r.instructions["concat"];

        auto concat_arg = std::find(ins->inputs().begin(), ins->inputs().end(), concat_ins) -
                          ins->inputs().begin();
        std::vector<instruction_ref> inputs;
        for(auto input : concat_ins->inputs())
            inputs.insert(inputs.end(), input->inputs().begin(), input->inputs().end());
        std::copy_if(ins->inputs().begin(),
                     ins->inputs().end(),
                     std::back_inserter(inputs),
                     [&](auto input) { return input != concat_ins; });

        std::vector<module_ref> module_inputs;
        std::transform(concat_ins->inputs().begin(),
                       concat_ins->inputs().end(),
                       std::back_inserter(module_inputs),
                       [&](instruction_ref input) {
                           if(input->name() == "pointwise")
                           {
                               auto* pm = input->module_inputs().front();
                               return mpm.create_module("concat:" + pm->name(), *pm);
                           }
                           auto* pm = mpm.create_module("concat" + std::to_string(counter++));

                           auto x  = pm->add_parameter("x", shape{input->get_shape().type()});
                           auto id = pm->add_instruction(make_op("identity"), x);
                           pm->add_return({id});
                           return pm;
                       });

        auto* post_pm                  = ins->module_inputs().front();
        auto* rm                       = mpm.create_module(post_pm->name() + ":concat", *post_pm);
        std::vector<std::string> names = rm->get_parameter_names();
        std::sort(names.begin(), names.end());
        auto concat_param_name = names[concat_arg];
        auto concat_param      = rm->get_parameter(concat_param_name);
        auto param = rm->add_parameter("!" + concat_param_name, concat_param->get_shape());
        rm->replace_instruction(concat_param, param);
        rm->remove_instruction(concat_param);

        module_inputs.push_back(rm);

        mpm.get_module().replace_instruction(
            ins,
            make_op("fused_concat", concat_ins->normalized_operator().to_value()),
            inputs,
            module_inputs);
    }
};

} // namespace

void fuse_concat::apply(module_pass_manager& mpm) const
{
    match::find_matches(mpm, find_pointwise_concat_pointwise{});
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
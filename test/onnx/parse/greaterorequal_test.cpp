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

#include <onnx_test.hpp>

TEST_CASE(greaterorequal_test)
{
    migraphx::program p;
    auto* mm = p.get_main_module();

    auto input1 = mm->add_parameter("x1", migraphx::shape{migraphx::shape::float_type, {3}});
    auto input2 = mm->add_parameter("x2", migraphx::shape{migraphx::shape::float_type, {3}});
    auto temp   = mm->add_instruction(migraphx::make_op("less"), input1, input2);
    auto bt     = mm->add_instruction(
        migraphx::make_op("convert", {{"target_type", migraphx::shape::bool_type}}), temp);
    auto ge = mm->add_instruction(migraphx::make_op("not"), bt);

    mm->add_return({ge});

    auto prog = read_onnx("greaterorequal_test.onnx");
    EXPECT(p == prog);
}

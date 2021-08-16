#pragma once

#include <ATen/ATen.h>

namespace torch {
namespace utils {

TORCH_API void check_out_type_matches(
    const at::Tensor& result,
    c10::optional<at::ScalarType>,
    c10::optional<at::Layout> layout,
    c10::optional<at::Device> device);

}}

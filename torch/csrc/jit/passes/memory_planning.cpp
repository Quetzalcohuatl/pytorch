#include <torch/csrc/jit/passes/memory_planning.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_breadth.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_size.h>
#include <torch/csrc/jit/passes/memory_planning/linear_scan.h>

#include <jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/runtime/static/ops.h>

namespace torch {
namespace jit {

c10::optional<uint64_t> computeStorageSize(const Value& value) {
  auto ttp = value.type()->cast<TensorType>();
  if (!ttp) {
    TORCH_WARN("out isn't a tensortype ", *value.type());
    return c10::nullopt;
  }
  if (!ttp->scalarType().has_value()) {
    TORCH_WARN(
        "This output was profiled but didn't have a scalar type: ",
        *ttp,
        ", ",
        value.debugName());
    return c10::nullopt;
  }
  if (!ttp->sizes().concrete_sizes().has_value()) {
    TORCH_WARN(
        "This output was profiled but doesn't have sizes: ",
        *ttp,
        ", ",
        value.debugName());
    return c10::nullopt;
  }

  auto scalar_type = ttp->scalarType();
  if (!scalar_type.has_value()) {
    TORCH_WARN(
        "This value doesn't have a scalar type", *ttp, ", ", value.debugName());
    return c10::nullopt;
  }

  auto element_size = c10::elementSize(scalar_type.value());
  // TODO: when does this fail? answer: in place mutation
  auto numel = ttp->numel();
  if (!numel.has_value()) {
    TORCH_WARN("doesn't have numel", *ttp, ", ", value.debugName());
    return c10::nullopt;
  }

  return numel.value() * element_size;
}

std::pair<std::vector<int64_t>, std::vector<int64_t>> getSizesStrides(
    const c10::TensorTypePtr& ttp) {
  std::vector<int64_t> sizes;
  auto _sizes = ttp->sizes().concrete_sizes();
  // TODO: why does this break? answer: in place mutation
  // also %9 : Long(requires_grad=0, device=cpu) = prim::Constant[value={0}]()
  if (_sizes.has_value() && _sizes.value().size() > 0 &&
      _sizes.value()[0] != 0) {
    sizes = _sizes.value();
  } else {
    sizes = std::vector<int64_t>{0};
  }
  std::vector<int64_t> strides;
  auto _strides = ttp->strides().concrete_sizes();
  if (_strides.has_value() && _strides.value().size() > 0 &&
      _strides.value()[0] != 0) {
    strides = _strides.value();
  } else {
    strides = at::detail::defaultStrides(sizes);
  }
  return std::make_pair(sizes, strides);
}

Node* insertAllocStorageNode(
    std::shared_ptr<Graph>& graph,
    uint64_t total_size) {
  auto* storage = graph->create(prim::AllocateStorage, 1);
  storage->i_(attr::total_size, total_size);

  auto deviceType = jit::tensorexpr::pickDeviceType(graph);
  if (deviceType.has_value()) {
    storage->i_(attr::device, static_cast<int8_t>(deviceType.value().type()));
  } else {
    storage->i_(attr::device, static_cast<int8_t>(at::kCPU));
  }
  storage->insertBefore(graph->nodes().front());
  return storage;
}

void insertAllocTensorNodes(
    std::shared_ptr<Graph>& graph,
    Node* storage,
    std::unordered_map<const Value*, Region> allocations) {
  uint64_t total_size = storage->i(attr::total_size);
  for (auto& allocation : allocations) {
    // const_cast fishy?
    auto node = const_cast<Node*>(allocation.first->node());
    auto region = allocation.second;

    // the way that this node magically *becomes* the out varaint is simply
    // by add an extra input. this is because op resolution happens
    // at runtime via the op registry (by matching on the schema).
    auto* alloc = graph->create(prim::AllocateTensor, 1);
    node->addInput(alloc->output());
    GRAPH_DEBUG("inserting allocation op for ", node->getOperator().schema());
    alloc->insertBefore(node);
    alloc->addInput(storage->output());

    auto ttp = allocation.first->type()->expect<c10::TensorType>();
    std::vector<int64_t> sizes, strides;
    std::tie(sizes, strides) = getSizesStrides(ttp);
    TORCH_CHECK(
        region.offset + region.size <= total_size,
        "trying to create an allocation that exceeds previously planned memory");
    alloc->i_(attr::size, region.size);
    alloc->i_(attr::offset, region.offset);
    alloc->is_(attr::sizes, sizes);
    alloc->is_(attr::stride, strides);
    alloc->i_(attr::device, static_cast<int8_t>(storage->i(attr::device)));
    alloc->i_(attr::dtype, static_cast<int8_t>(ttp->scalarType().value()));
  }
}

bool hasOutVariant(Node* node) {
  for (const auto& variant : getAllOperatorsFor(node->kind())) {
    auto variant_args = variant->schema().arguments();
    /* TODO
      aten::cat.names_out(Tensor[] tensors, str dim, *, Tensor(a!) out) ->
      (Tensor(a!)) aten::cat.out(Tensor[] tensors, int dim=0, *,
      Tensor(a!) out) -> (Tensor(a!))
    */
    auto maybe_out_arg =
        std::find_if(variant_args.begin(), variant_args.end(), [](auto arg) {
          return arg.name() == "out";
        });
    if (maybe_out_arg != variant_args.end()) {
      return true;
    }
  }
  return false;
}

std::pair<std::vector<const Node*>, std::unordered_map<const Value*, uint64_t>>
getManagedValues(
    const std::shared_ptr<Graph>& graph,
    std::unordered_set<const Value*> always_alive_values) {
  std::unordered_map<const Value*, uint64_t> managed_tensor_values;
  std::unordered_set<const Value*> leaked_values;
  std::vector<const Node*> out_nodes;

  for (auto node : graph->nodes()) {
    if (!hasOutVariant(node)) {
      continue;
    }
    out_nodes.emplace_back(node);
    for (const auto& out_v : node->outputs()) {
      if (always_alive_values.count(out_v)) {
        continue;
      }
      auto size = computeStorageSize(*out_v);
      if (size.has_value() && size.value() > 0) {
        managed_tensor_values.insert({out_v, size.value()});
      } else if (isOptimizableContainerType(node)) {
        leaked_values.insert(out_v);
      } else {
        TORCH_WARN(
            "not handling unsupported value: ",
            out_v->debugName(),
            " ",
            *out_v->type());
        leaked_values.insert(out_v);
      }
    }
  }
  return std::make_pair(out_nodes, managed_tensor_values);
}

std::tuple<
    std::vector<const Node*>,
    std::unordered_map<const Value*, uint64_t>,
    std::unordered_map<const Value*, LiveRange>>
getManagedStuff(std::shared_ptr<Graph>& graph) {
  AliasDb alias_db(graph);
  auto always_alive = jit::GetAlwaysAliveValues(graph, alias_db);
  auto live_ranges = jit::GetLiveness(graph, always_alive, alias_db).second;
  std::vector<const Node*> out_nodes;
  std::unordered_map<const Value*, uint64_t> managed_tensor_values;
  std::tie(out_nodes, managed_tensor_values) =
      getManagedValues(graph, always_alive);

  std::unordered_map<const Value*, LiveRange> managed_ranges;
  for (const auto& lvr : live_ranges) {
    if (managed_tensor_values.count(lvr.first) > 0) {
      managed_ranges.insert(lvr);
    }
  }
  return std::make_tuple(out_nodes, managed_tensor_values, managed_ranges);
}

uint64_t getTotalAllocationSize(
    std::unordered_map<const Value*, Region> allocations) {
  uint64_t total_size = 0;
  for (const auto& item : allocations) {
    total_size = std::max(total_size, item.second.offset + item.second.size);
  }
  return total_size;
}

void printAllocation(
    std::unordered_map<const Value*, Region> allocations,
    std::unordered_map<const Value*, LiveRange> managed_ranges) {
  std::map<LiveRange, const Value*, live_range_start_comp>
      sorted_start_live_ranges_map;
  for (const auto& item : managed_ranges) {
    sorted_start_live_ranges_map.insert({item.second, item.first});
  }

  for (const auto& item : sorted_start_live_ranges_map) {
    auto lvr = item.first;
    auto val = item.second;
    auto alloced_reg = allocations[val];
    std::cout << val->debugName() << ": " << lvr << " " << alloced_reg << "\n";
  }
}

void planMemory(std::shared_ptr<Graph>& graph, Strategy strat) {
  std::unordered_map<const Value*, uint64_t> managed_values;
  std::unordered_map<const Value*, LiveRange> managed_ranges;
  std::vector<const Node*> out_nodes;
  std::tie(out_nodes, managed_values, managed_ranges) = getManagedStuff(graph);
  std::unordered_map<const Value*, Region> allocations;

  switch (strat) {
    case Strategy::NAIVE: {
      return;
    }
    case Strategy::GREEDY_BY_SIZE: {
      allocations = greedyBySize(managed_values, managed_ranges);
      break;
    }
    case Strategy::LINEAR_SCAN: {
      allocations = linearScanHeuristic(managed_values, managed_ranges);
      break;
    };
    case Strategy::GREEDY_BY_BREADTH: {
      allocations =
          greedyByOperatorBreadth(managed_values, managed_ranges, out_nodes);
      break;
    };
    default:
      return;
  }
  auto total_size = getTotalAllocationSize(allocations);

  printAllocation(allocations, managed_ranges);

  GRAPH_DEBUG("\ngraph before inserting storage node\n", *graph);

  auto storage_node = insertAllocStorageNode(graph, total_size);
  GRAPH_DEBUG("\ngraph after inserting storage node\n", *graph);

  insertAllocTensorNodes(graph, storage_node, allocations);
  GRAPH_DEBUG("\ngraph after inserting alloc nodes\n", *graph);
}
} // namespace jit
} // namespace torch

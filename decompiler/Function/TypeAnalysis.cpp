#include "decompiler/Function/Function.h"
#include "decompiler/IR/IR.h"
#include "third-party/fmt/core.h"
#include "decompiler/config.h"

namespace decompiler {
namespace {
TypeState construct_initial_typestate(const TypeSpec& f_ts) {
  TypeState result;
  int goal_args[] = {Reg::A0, Reg::A1, Reg::A2, Reg::A3, Reg::T0, Reg::T1, Reg::T2, Reg::T3};
  assert(f_ts.base_type() == "function");
  assert(f_ts.arg_count() >= 1);
  assert(f_ts.arg_count() <= 8 + 1);  // 8 args + 1 return.
  for (int i = 0; i < int(f_ts.arg_count()) - 1; i++) {
    auto reg_id = goal_args[i];
    auto reg_type = f_ts.get_arg(i);
    result.gpr_types[reg_id] = TP_Type::make_from_ts(reg_type);
  }

  // todo, more specific process types for behaviors.
  result.gpr_types[Reg::S6] = TP_Type::make_from_ts(TypeSpec("process"));
  return result;
}

/*!
 * Modify the the given type state based on the given casts.
 */
void modify_input_types_for_casts(
    const std::vector<TypeCast>& casts,
    TypeState* state,
    std::unordered_map<Register, TP_Type, Register::hash>* changed_types,
    DecompilerTypeSystem& dts) {
  for (auto& cast : casts) {
    try {
      auto type_from_cast = TP_Type::make_from_ts(dts.parse_type_spec(cast.type_name));
      auto original_type = state->get(cast.reg);
      // fmt::print("Cast reg {} : {} -> {}\n", cast.reg.to_string(), original_type.print(),
      // type_from_cast.print());
      if (original_type != type_from_cast) {
        // the cast will have an effect on types. If we are removing the original type, remember it
        if (changed_types && changed_types->find(cast.reg) == changed_types->end()) {
          (*changed_types)[cast.reg] = original_type;
        }

        // and actually update the typ
        state->get(cast.reg) = type_from_cast;
      }
    } catch (std::exception& e) {
      printf("failed to parse hint: %s\n", e.what());
      assert(false);
    }
  }
}

void try_modify_input_types_for_casts(
    int idx,
    const std::unordered_map<int, std::vector<TypeCast>>& casts,
    TypeState* state,
    std::unordered_map<Register, TP_Type, Register::hash>* changed_types,
    DecompilerTypeSystem& dts) {
  auto kv = casts.find(idx);
  if (kv != casts.end()) {
    // fmt::print("at idx {}, casting:\n", idx);
    modify_input_types_for_casts(kv->second, state, changed_types, dts);
  }
}
}  // namespace

bool Function::run_type_analysis_ir2(
    const TypeSpec& my_type,
    DecompilerTypeSystem& dts,
    LinkedObjectFile& file,
    const std::unordered_map<int, std::vector<TypeCast>>& casts,
    const std::unordered_map<std::string, LabelType>& label_types) {
  (void)file;
  ir2.env.set_type_casts(casts);
  ir2.env.set_label_types(label_types);
  // STEP 0 - set decompiler type system settings for this function. In config we can manually
  // specify some settings for type propagation to reduce the strictness of type propagation.
  // TODO - this is kinda hacky so that it works in both unit tests and actual decompilation.
  // it would be better if this setting came 100% from the IR2 env.
  if (!dts.type_prop_settings.locked) {
    dts.type_prop_settings.reset();
    if (get_config().pair_functions_by_name.find(guessed_name.to_string()) !=
        get_config().pair_functions_by_name.end()) {
      dts.type_prop_settings.allow_pair = true;
      ir2.env.set_sloppy_pair_typing();
    }
  } else {
    if (dts.type_prop_settings.allow_pair) {
      ir2.env.set_sloppy_pair_typing();
    }
  }

  if (guessed_name.kind == FunctionName::FunctionKind::METHOD) {
    dts.type_prop_settings.current_method_type = guessed_name.type_name;
  }

  if (my_type.last_arg() == TypeSpec("none")) {
    auto as_end = dynamic_cast<FunctionEndOp*>(ir2.atomic_ops->ops.back().get());
    assert(as_end);
    as_end->mark_function_as_no_return_value();
  }

  std::vector<TypeState> block_init_types, op_types;
  block_init_types.resize(basic_blocks.size());
  op_types.resize(ir2.atomic_ops->ops.size());
  auto& aop = ir2.atomic_ops;

  // STEP 1 - topologocial sort the blocks. This gives us an order where we:
  // - never visit unreachable blocks (we can't type propagate these)
  // - always visit at least one predecessor of a block before that block
  auto order = bb_topo_sort();
  assert(!order.vist_order.empty());
  assert(order.vist_order.front() == 0);

  // STEP 2 - initialize type state for the first block to the function argument types.
  block_init_types.at(0) = construct_initial_typestate(my_type);
  // and add hints from config
  // TODO - this should be moved into the construct_initial_typestate
  //  try_apply_hints(0, hints, &block_init_types.at(0), dts);

  // STEP 3 - propagate types until the result stops changing
  bool run_again = true;
  while (run_again) {
    run_again = false;
    // do each block in the topological sort order:
    for (auto block_id : order.vist_order) {
      auto& block = basic_blocks.at(block_id);
      TypeState* init_types = &block_init_types.at(block_id);
      for (int op_id = aop->block_id_to_first_atomic_op.at(block_id);
           op_id < aop->block_id_to_end_atomic_op.at(block_id); op_id++) {
        // apply type hints only if we are not the first op.

        std::unordered_map<Register, TP_Type, Register::hash> restore_cast_types;
        // if (op_id != aop->block_id_to_first_atomic_op.at(block_id)) {
        try_modify_input_types_for_casts(op_id, casts, init_types, &restore_cast_types, dts);
        //}

        auto& op = aop->ops.at(op_id);

        try {
          op_types.at(op_id) = op->propagate_types(*init_types, ir2.env, dts);
        } catch (std::runtime_error& e) {
          lg::warn("Function {} failed type prop: {}", guessed_name.to_string(), e.what());
          warnings.type_prop_warning("{}", e.what());
          ir2.env.set_types(block_init_types, op_types, *ir2.atomic_ops);
          return false;
        }

        // undo casts.
        for (auto& restore_kv : restore_cast_types) {
          if (std::find(op->write_regs().begin(), op->write_regs().end(), restore_kv.first) ==
              op->write_regs().end()) {
            op_types.at(op_id).get(restore_kv.first) = restore_kv.second;
          }

          init_types->get(restore_kv.first) = restore_kv.second;
        }

        // for the next op...
        init_types = &op_types.at(op_id);
      }

      // propagate the types: for each possible succ
      for (auto succ_block_id : {block.succ_ft, block.succ_branch}) {
        if (succ_block_id != -1) {
          // apply hint
          //          try_apply_hints(aop->block_id_to_first_atomic_op.at(succ_block_id), hints,
          //          init_types,
          //                          dts);

          // set types to LCA (current, new)
          if (dts.tp_lca(&block_init_types.at(succ_block_id), *init_types)) {
            // if something changed, run again!
            run_again = true;
          }
        }
      }
    }
  }

  auto last_type = op_types.back().get(Register(Reg::GPR, Reg::V0)).typespec();
  if (last_type != my_type.last_arg()) {
    warnings.info("Return type mismatch {} vs {}.", last_type.print(), my_type.last_arg().print());
  }

  // and apply final casts:
  for (auto block_id : order.vist_order) {
    for (int op_id = aop->block_id_to_first_atomic_op.at(block_id);
         op_id < aop->block_id_to_end_atomic_op.at(block_id); op_id++) {
      if (op_id == aop->block_id_to_first_atomic_op.at(block_id)) {
        try_modify_input_types_for_casts(op_id, casts, &block_init_types.at(block_id), nullptr,
                                         dts);
      } else {
        try_modify_input_types_for_casts(op_id, casts, &op_types.at(op_id - 1), nullptr, dts);
      }
    }
  }

  ir2.env.set_types(block_init_types, op_types, *ir2.atomic_ops);

  return true;
}
}  // namespace decompiler

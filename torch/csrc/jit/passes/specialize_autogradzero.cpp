#include <torch/csrc/jit/passes/specialize_autogradzero.h>
#include <torch/csrc/jit/symbolic_variable.h>

namespace torch {
namespace jit {

// propagate autograd zero information through a gradient graph and
// remove grad_of blocks if present.
// Note: this is a very limited pass. It only propagates autograd zeros for
// operations generated by the symbolic autodiff code and cleans up
// AutogradAdds when possible. Outputs of other nodes are conservatively
// marked Unknown and not optimized.
void specializeAutogradZero(Graph& g) {
  enum class State { Nonzero, Zero, Unknown };
  std::unordered_map<Value*, State> state;

  for (Value* input : g.inputs()) {
    const auto& tp = input->type();
    if (auto tt = tp->cast<TensorType>()) {
      if (tt->autogradZero() && *tt->autogradZero()) {
        state[input] = State::Zero;
      } else {
        state[input] = State::Nonzero;
      }
    } else if (
        tp->isSubtypeOf(TensorType::get()) ||
        tp->isSubtypeOf(ListType::ofTensors())) {
      state[input] = State::Nonzero;
    } else {
      state[input] = State::Unknown;
    }
  }

  for (auto it = g.nodes().begin(); it != g.nodes().end(); ++it) {
    auto n = *it;
    switch (n->kind()) {
      case prim::GradOf: {
        auto all_zeros =
            std::all_of(n->inputs().begin(), n->inputs().end(), [&](Value* v) {
              return state[v] == State::Zero;
            });
        // Property 1: if all the gradInputs to the GradOf are Zero
        // then the gradOutputs are also zero and will be represented as
        // AutogradZero nodes
        if (all_zeros) {
          auto zero = g.createAutogradZero()->insertAfter(n)->output();
          for (auto o : n->outputs()) {
            o->replaceAllUsesWith(zero);
          }
        } else {
          // Property 2: GradOfs are required to correctly handle combinations
          // of Nonzero and zero inputs. They are expected to produce
          // Nonzero output tensors in this case.

          // Remove the GradOf, splicing its body back into the surrounding
          // block
          auto body = n->blocks().at(0);
          for (auto input : n->inputs()) {
            // we should never get into a situation when specializing a GradOf
            // where we do not know if a value is Nonzero since at the top level
            // a gradient graph is composed of Linear nodes and AutogradAdds
            // and LinearNodes only appear in these graphs
            AT_ASSERT(state[input] != State::Unknown);
          }
          // hoist the nodes in the GradOf body to be before the linear block
          for (auto it = body->nodes().begin(); it != body->nodes().end();) {
            auto block_node = *it++;
            block_node->moveBefore(n);
          }

          for (size_t i = 0; i < n->outputs().size(); ++i)
            n->outputs().at(i)->replaceAllUsesWith(body->outputs().at(i));
        }
        it.destroyCurrent();
      } break;
      case prim::AutogradAdd: {
        auto a = n->input(0);
        auto b = n->input(1);
        // if one is Autograd zero, we can just drop the add
        if (state[a] == State::Zero) {
          // Zero + b == b
          n->output()->replaceAllUsesWith(b);
          it.destroyCurrent();
        } else if (state[b] == State::Zero) {
          // a + Zero == a
          n->output()->replaceAllUsesWith(a);
          it.destroyCurrent();
        } else if (state[a] == State::Nonzero && state[b] == State::Nonzero) {
          // when both are Nonzero, we can use a normal, optimizable add
          // instruction
          WithInsertPoint guard(n);
          Value* new_add = toVar(a) + toVar(b);
          state[new_add] = State::Nonzero;
          n->output()->replaceAllUsesWith(new_add);
          it.destroyCurrent();
        } else {
          // otherwise we have conditionally-Nonzero things, and we need
          // to actually run an AutogradAdd which will guard for Zeros
          // so we leave the op as is
          state[n->output()] = State::Unknown;
        }
      } break;
      case prim::AutogradZero: {
        state[n->output()] = State::Zero;
      } break;
      default:
        for (auto o : n->outputs()) {
          state[o] = State::Unknown;
        }
        break;
    }
  }
}

} // namespace jit
} // namespace torch

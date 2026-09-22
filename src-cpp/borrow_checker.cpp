#include "borrow_checker.h"
#include "error.h"

#include <algorithm>
#include <iostream>
#include <sstream>

// =========================================================================
// Helpers
// =========================================================================

void NLLBorrowChecker::set_error(const std::string &msg, Expr *expr) {
  if (has_error_) return;
  has_error_ = true;
  if (expr && expr->line > 0)
    error_msg_ = error_at(expr->file, expr->line, expr->col, msg);
  else
    error_msg_ = "error: " + msg;
}

// =========================================================================
// Cross-function lookup helpers
// =========================================================================

void NLLBorrowChecker::set_declarations(
    const std::vector<std::unique_ptr<Decl>> &decls) {
  fn_decls.clear();
  extern_decls.clear();
  impl_decls.clear();
  for (auto &decl : decls) {
    if (auto *fn = dynamic_cast<FnDecl *>(decl.get())) {
      if (fn->is_extern)
        extern_decls[fn->name] = fn;
      else
        fn_decls[fn->name] = fn;
    }
    if (auto *impl = dynamic_cast<ImplDecl *>(decl.get())) {
      impl_decls.push_back(impl);
    }
  }
}

FnDecl *NLLBorrowChecker::lookup_fn(const std::string &name) {
  auto it = fn_decls.find(name);
  if (it != fn_decls.end()) return it->second;
  auto it2 = extern_decls.find(name);
  if (it2 != extern_decls.end()) return it2->second;
  return nullptr;
}

// =========================================================================
// Static helpers
// =========================================================================

static std::string root_var(Expr *expr) {
  if (!expr) return "";
  if (auto *id = dynamic_cast<IdentExpr *>(expr))
    return id->name;
  if (auto *field = dynamic_cast<FieldAccessExpr *>(expr))
    return root_var(field->object.get());
  if (auto *deref = dynamic_cast<DerefExpr *>(expr))
    return root_var(deref->operand.get());
  if (auto *sub = dynamic_cast<SubscriptExpr *>(expr))
    return root_var(sub->array.get());
  if (auto *unary = dynamic_cast<UnaryExpr *>(expr))
    return root_var(unary->operand.get());
  return "";
}

static bool is_ref_type(const TypeAnnotation &ann) {
  return ann.kind == TypeKind::Ref || ann.kind == TypeKind::MutRef;
}

static bool is_mut_ref_type(const TypeAnnotation &ann) {
  return ann.kind == TypeKind::MutRef;
}

// =========================================================================
// Step 1: Walk AST to find borrow expressions and map them to CFG nodes.
//
// For explicit borrows (&x, &mut x) found in the AST, register a
// BorrowRegion. For CallExpr and MethodCallExpr, check callee parameter
// types to register implicit borrows (cross-function checking).
// =========================================================================

void NLLBorrowChecker::collect_borrows_expr(
    Expr *expr, int node_id, const std::string &ref_var) {
  if (!expr || has_error_) return;

  // --- Explicit borrow expression ---
  if (auto *borrow = dynamic_cast<BorrowExpr *>(expr)) {
    std::string var = root_var(borrow->operand.get());
    if (!var.empty()) {
      BorrowRegion br;
      br.var_name = var;
      br.ref_var = ref_var;
      br.is_mut = borrow->is_mut;
      br.create_node = node_id;
      br.end_node = node_id;
      br.expr = expr;
      borrows.push_back(br);
    }
    return;
  }

  // --- Named function call: cross-function borrow checking ---
  if (auto *call = dynamic_cast<CallExpr *>(expr)) {
    // First, recurse into arguments to find explicit borrows
    for (auto &arg : call->args)
      collect_borrows_expr(arg.get(), node_id, ref_var);

    // Then, check if callee takes &T/&mut T params → implicit borrows
    if (!call->callee.empty()) {
      if (FnDecl *callee = lookup_fn(call->callee)) {
        for (size_t i = 0; i < call->args.size() && i < callee->params.size(); i++) {
          auto &param = callee->params[i];
          if (is_ref_type(param.type_ann)) {
            std::string var = root_var(call->args[i].get());
            if (!var.empty()) {
              // Check if there's already an explicit borrow for this arg
              bool has_explicit = false;
              for (auto &br : borrows) {
                if (br.var_name == var && br.create_node == node_id) {
                  has_explicit = true;
                  break;
                }
              }
              if (!has_explicit) {
                BorrowRegion br;
                br.var_name = var;
                br.ref_var = "";
                br.is_mut = is_mut_ref_type(param.type_ann);
                br.create_node = node_id;
                br.end_node = node_id;
                br.expr = call->args[i].get();
                borrows.push_back(br);
              }
            }
          }
        }
      }
    }
    return;
  }

  // --- Method call: self is implicitly borrowed ---
  if (auto *mcall = dynamic_cast<MethodCallExpr *>(expr)) {
    // Recurse into args first
    for (auto &arg : mcall->args)
      collect_borrows_expr(arg.get(), node_id, ref_var);

    // Resolve the method's self parameter type
    // We need the type of mcall->object, but we don't have type info here.
    // Heuristic: look up all impls that have this method name.
    // If the first param is &self → shared borrow; &mut self → mutable borrow.
    bool found_method = false;
    for (auto *impl : impl_decls) {
      for (auto &m : impl->methods) {
        if (m->name == mcall->method_name && !m->params.empty()) {
          auto &self_param = m->params[0];
          if (is_ref_type(self_param.type_ann)) {
            std::string var = root_var(mcall->object.get());
            if (!var.empty() && !found_method) {
              BorrowRegion br;
              br.var_name = var;
              br.ref_var = "";
              br.is_mut = is_mut_ref_type(self_param.type_ann);
              br.create_node = node_id;
              br.end_node = node_id;
              br.expr = mcall->object.get();
              borrows.push_back(br);
              found_method = true;
            }
          }
        }
      }
    }

    // Also recurse into object (for cases like foo().method())
    collect_borrows_expr(mcall->object.get(), node_id, ref_var);
    return;
  }

  // --- Recurse into sub-expressions ---
  if (auto *bin = dynamic_cast<BinaryExpr *>(expr)) {
    collect_borrows_expr(bin->left.get(), node_id, ref_var);
    collect_borrows_expr(bin->right.get(), node_id, ref_var);
    return;
  }
  if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
    collect_borrows_expr(unary->operand.get(), node_id, ref_var);
    return;
  }
  if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
    collect_borrows_expr(assign->value.get(), node_id, ref_var);
    return;
  }
  if (auto *compound = dynamic_cast<CompoundAssignExpr *>(expr)) {
    collect_borrows_expr(compound->value.get(), node_id, ref_var);
    return;
  }
  if (auto *deref = dynamic_cast<DerefExpr *>(expr)) {
    collect_borrows_expr(deref->operand.get(), node_id, ref_var);
    return;
  }
  if (auto *sub = dynamic_cast<SubscriptExpr *>(expr)) {
    collect_borrows_expr(sub->array.get(), node_id, ref_var);
    collect_borrows_expr(sub->index.get(), node_id, ref_var);
    return;
  }
  if (auto *field = dynamic_cast<FieldAccessExpr *>(expr)) {
    collect_borrows_expr(field->object.get(), node_id, ref_var);
    return;
  }
  if (auto *arr = dynamic_cast<ArrayLitExpr *>(expr)) {
    for (auto &el : arr->elements)
      collect_borrows_expr(el.get(), node_id, ref_var);
    return;
  }
  if (auto *tup = dynamic_cast<TupleExpr *>(expr)) {
    for (auto &el : tup->elements)
      collect_borrows_expr(el.get(), node_id, ref_var);
    return;
  }
  if (auto *ctor = dynamic_cast<ConstructorExpr *>(expr)) {
    for (auto &[_, fexpr] : ctor->fields)
      collect_borrows_expr(fexpr.get(), node_id, ref_var);
    return;
  }
  if (auto *match = dynamic_cast<MatchExpr *>(expr)) {
    collect_borrows_expr(match->value.get(), node_id, ref_var);
    for (auto &arm : match->arms) {
      collect_borrows_expr(arm.expr.get(), node_id, ref_var);
    }
    return;
  }
  if (auto *ifexpr = dynamic_cast<IfExpr *>(expr)) {
    collect_borrows_expr(ifexpr->condition.get(), node_id, ref_var);
    collect_borrows_expr(ifexpr->then_expr.get(), node_id, ref_var);
    collect_borrows_expr(ifexpr->else_expr.get(), node_id, ref_var);
    return;
  }
  if (auto *atm = dynamic_cast<AtomicExpr *>(expr)) {
    for (auto &arg : atm->args)
      collect_borrows_expr(arg.get(), node_id, ref_var);
    return;
  }
  if (auto *closure = dynamic_cast<ClosureExpr *>(expr)) {
    // Closures capture by value — don't borrow from enclosing scope
    return;
  }
}

// =========================================================================
// Statement walker for borrow collection
// =========================================================================

void NLLBorrowChecker::collect_borrows(Stmt *stmt, CFG &cfg, int node_id) {
  if (!stmt) return;

  if (auto *expr_s = dynamic_cast<ExprStmt *>(stmt)) {
    collect_borrows_expr(expr_s->expr.get(), node_id, "");
    return;
  }
  if (auto *let = dynamic_cast<LetStmt *>(stmt)) {
    if (let->init_expr)
      collect_borrows_expr(let->init_expr.get(), node_id, let->name);
    return;
  }
  if (auto *ret = dynamic_cast<ReturnStmt *>(stmt)) {
    if (ret->value)
      collect_borrows_expr(ret->value.get(), node_id, "");
    return;
  }
  if (auto *ifs = dynamic_cast<IfStmt *>(stmt)) {
    collect_borrows_expr(ifs->condition.get(), node_id, "");
    for (auto &s : ifs->then_branch)
      collect_borrows(s.get(), cfg, node_id);
    for (auto &s : ifs->else_branch)
      collect_borrows(s.get(), cfg, node_id);
    return;
  }
  if (auto *for_s = dynamic_cast<ForStmt *>(stmt)) {
    if (for_s->init) collect_borrows(for_s->init.get(), cfg, node_id);
    if (for_s->condition)
      collect_borrows_expr(for_s->condition.get(), node_id, "");
    if (for_s->update)
      collect_borrows_expr(for_s->update.get(), node_id, "");
    for (auto &s : for_s->body)
      collect_borrows(s.get(), cfg, node_id);
    return;
  }
  if (auto *while_s = dynamic_cast<WhileStmt *>(stmt)) {
    if (while_s->condition)
      collect_borrows_expr(while_s->condition.get(), node_id, "");
    for (auto &s : while_s->body)
      collect_borrows(s.get(), cfg, node_id);
    return;
  }
  if (auto *region = dynamic_cast<RegionStmt *>(stmt)) {
    for (auto &s : region->body)
      collect_borrows(s.get(), cfg, node_id);
    return;
  }
  if (auto *brk = dynamic_cast<BreakStmt *>(stmt)) {
    return;
  }
  if (auto *cont = dynamic_cast<ContinueStmt *>(stmt)) {
    return;
  }
}

// =========================================================================
// Closure body checking
// =========================================================================

bool NLLBorrowChecker::check_closures_in_expr(Expr *expr) {
  if (!expr) return false;

  if (auto *closure = dynamic_cast<ClosureExpr *>(expr)) {
    NLLBorrowChecker inner;
    inner.set_declarations(fn_decls.size() > 0
        ? std::vector<std::unique_ptr<Decl>>() // can't move; share the maps
        : std::vector<std::unique_ptr<Decl>>());
    // Share declaration maps with inner checker
    inner.fn_decls = fn_decls;
    inner.extern_decls = extern_decls;
    inner.impl_decls = impl_decls;
    if (!inner.check_body("<closure>", closure->body)) {
      has_error_ = true;
      error_msg_ = inner.error();
      return true;
    }
    return false;
  }

  if (auto *bin = dynamic_cast<BinaryExpr *>(expr)) {
    if (check_closures_in_expr(bin->left.get())) return true;
    if (check_closures_in_expr(bin->right.get())) return true;
    return false;
  }
  if (auto *call = dynamic_cast<CallExpr *>(expr)) {
    if (check_closures_in_expr(call->callee_expr.get())) return true;
    for (auto &arg : call->args)
      if (check_closures_in_expr(arg.get())) return true;
    return false;
  }
  if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
    if (check_closures_in_expr(assign->value.get())) return true;
    return false;
  }
  if (auto *ifexpr = dynamic_cast<IfExpr *>(expr)) {
    if (check_closures_in_expr(ifexpr->condition.get())) return true;
    if (check_closures_in_expr(ifexpr->then_expr.get())) return true;
    if (check_closures_in_expr(ifexpr->else_expr.get())) return true;
    return false;
  }
  if (auto *match = dynamic_cast<MatchExpr *>(expr)) {
    if (check_closures_in_expr(match->value.get())) return true;
    for (auto &arm : match->arms)
      if (check_closures_in_expr(arm.expr.get())) return true;
    return false;
  }
  if (auto *mcall = dynamic_cast<MethodCallExpr *>(expr)) {
    if (check_closures_in_expr(mcall->object.get())) return true;
    for (auto &arg : mcall->args)
      if (check_closures_in_expr(arg.get())) return true;
    return false;
  }
  if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
    if (check_closures_in_expr(unary->operand.get())) return true;
    return false;
  }
  if (auto *compound = dynamic_cast<CompoundAssignExpr *>(expr)) {
    if (check_closures_in_expr(compound->target.get())) return true;
    if (check_closures_in_expr(compound->value.get())) return true;
    return false;
  }
  if (auto *borrow = dynamic_cast<BorrowExpr *>(expr)) {
    if (check_closures_in_expr(borrow->operand.get())) return true;
    return false;
  }
  if (auto *deref = dynamic_cast<DerefExpr *>(expr)) {
    if (check_closures_in_expr(deref->operand.get())) return true;
    return false;
  }
  if (auto *sub = dynamic_cast<SubscriptExpr *>(expr)) {
    if (check_closures_in_expr(sub->array.get())) return true;
    if (check_closures_in_expr(sub->index.get())) return true;
    return false;
  }
  if (auto *field = dynamic_cast<FieldAccessExpr *>(expr)) {
    if (check_closures_in_expr(field->object.get())) return true;
    return false;
  }
  if (auto *arr = dynamic_cast<ArrayLitExpr *>(expr)) {
    for (auto &el : arr->elements)
      if (check_closures_in_expr(el.get())) return true;
    return false;
  }
  if (auto *tup = dynamic_cast<TupleExpr *>(expr)) {
    for (auto &el : tup->elements)
      if (check_closures_in_expr(el.get())) return true;
    return false;
  }
  if (auto *ctor = dynamic_cast<ConstructorExpr *>(expr)) {
    for (auto &[_, fexpr] : ctor->fields)
      if (check_closures_in_expr(fexpr.get())) return true;
    return false;
  }
  if (auto *atm = dynamic_cast<AtomicExpr *>(expr)) {
    for (auto &arg : atm->args)
      if (check_closures_in_expr(arg.get())) return true;
    return false;
  }
  return false;
}

bool NLLBorrowChecker::check_closures_in_stmt(Stmt *stmt) {
  if (!stmt) return false;

  if (auto *expr_s = dynamic_cast<ExprStmt *>(stmt))
    return check_closures_in_expr(expr_s->expr.get());
  if (auto *let = dynamic_cast<LetStmt *>(stmt))
    if (let->init_expr)
      return check_closures_in_expr(let->init_expr.get());
  if (auto *ret = dynamic_cast<ReturnStmt *>(stmt))
    if (ret->value)
      return check_closures_in_expr(ret->value.get());
  if (auto *ifs = dynamic_cast<IfStmt *>(stmt)) {
    if (check_closures_in_expr(ifs->condition.get())) return true;
    for (auto &s : ifs->then_branch)
      if (check_closures_in_stmt(s.get())) return true;
    for (auto &s : ifs->else_branch)
      if (check_closures_in_stmt(s.get())) return true;
    return false;
  }
  if (auto *for_s = dynamic_cast<ForStmt *>(stmt)) {
    if (for_s->init && check_closures_in_stmt(for_s->init.get())) return true;
    if (for_s->condition && check_closures_in_expr(for_s->condition.get()))
      return true;
    if (for_s->update && check_closures_in_expr(for_s->update.get()))
      return true;
    for (auto &s : for_s->body)
      if (check_closures_in_stmt(s.get())) return true;
    return false;
  }
  if (auto *while_s = dynamic_cast<WhileStmt *>(stmt)) {
    if (while_s->condition && check_closures_in_expr(while_s->condition.get()))
      return true;
    for (auto &s : while_s->body)
      if (check_closures_in_stmt(s.get())) return true;
    return false;
  }
  if (auto *region = dynamic_cast<RegionStmt *>(stmt)) {
    for (auto &s : region->body)
      if (check_closures_in_stmt(s.get())) return true;
    return false;
  }
  return false;
}

// =========================================================================
// Step 2: Compute borrow lifetimes using liveness
// =========================================================================

void NLLBorrowChecker::compute_borrow_lifetimes(CFG &cfg) {
  for (auto &br : borrows) {
    const std::string &track_var = br.ref_var.empty() ? br.var_name : br.ref_var;

    if (br.ref_var.empty()) {
      // Inline borrow: lifetime is just the creation node
      br.end_node = br.create_node;
      continue;
    }

    // Let-bound borrow: find last use of the reference variable
    int last = -1;
    for (auto &node : cfg.nodes) {
      if (node.id < br.create_node) continue;
      if (node.gen.count(track_var) || node.kill.count(track_var) ||
          cfg.live_out[node.id].count(track_var)) {
        last = node.id;
      }
    }
    br.end_node = (last >= 0) ? last : br.create_node;
  }
}

// =========================================================================
// Step 3: Check borrow rules at each program point
// =========================================================================

static void collect_var_uses(Expr *expr, std::set<std::string> &reads,
                             std::set<std::string> &writes,
                             bool is_lvalue = false) {
  if (!expr) return;
  if (auto *id = dynamic_cast<IdentExpr *>(expr)) {
    if (is_lvalue) writes.insert(id->name);
    else reads.insert(id->name);
    return;
  }
  if (auto *bin = dynamic_cast<BinaryExpr *>(expr)) {
    collect_var_uses(bin->left.get(), reads, writes, false);
    collect_var_uses(bin->right.get(), reads, writes, false);
    return;
  }
  if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
    collect_var_uses(unary->operand.get(), reads, writes, false);
    return;
  }
  if (auto *call = dynamic_cast<CallExpr *>(expr)) {
    collect_var_uses(call->callee_expr.get(), reads, writes, false);
    for (auto &arg : call->args)
      collect_var_uses(arg.get(), reads, writes, false);
    return;
  }
  if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
    collect_var_uses(assign->value.get(), reads, writes, false);
    collect_var_uses(assign->target.get(), reads, writes, true);
    return;
  }
  if (auto *compound = dynamic_cast<CompoundAssignExpr *>(expr)) {
    collect_var_uses(compound->value.get(), reads, writes, false);
    collect_var_uses(compound->target.get(), reads, writes, false);
    collect_var_uses(compound->target.get(), reads, writes, true);
    return;
  }
  if (auto *deref = dynamic_cast<DerefExpr *>(expr)) {
    collect_var_uses(deref->operand.get(), reads, writes, false);
    return;
  }
  if (auto *sub = dynamic_cast<SubscriptExpr *>(expr)) {
    collect_var_uses(sub->array.get(), reads, writes, false);
    collect_var_uses(sub->index.get(), reads, writes, false);
    return;
  }
  if (auto *field = dynamic_cast<FieldAccessExpr *>(expr)) {
    collect_var_uses(field->object.get(), reads, writes, is_lvalue);
    return;
  }
  if (auto *arr = dynamic_cast<ArrayLitExpr *>(expr)) {
    for (auto &el : arr->elements)
      collect_var_uses(el.get(), reads, writes, false);
    return;
  }
  if (auto *tup = dynamic_cast<TupleExpr *>(expr)) {
    for (auto &el : tup->elements)
      collect_var_uses(el.get(), reads, writes, false);
    return;
  }
  if (auto *ctor = dynamic_cast<ConstructorExpr *>(expr)) {
    for (auto &[_, fexpr] : ctor->fields)
      collect_var_uses(fexpr.get(), reads, writes, false);
    return;
  }
  if (auto *match = dynamic_cast<MatchExpr *>(expr)) {
    collect_var_uses(match->value.get(), reads, writes, false);
    for (auto &arm : match->arms) {
      collect_var_uses(arm.expr.get(), reads, writes, false);
    }
    return;
  }
  if (auto *ifexpr = dynamic_cast<IfExpr *>(expr)) {
    collect_var_uses(ifexpr->condition.get(), reads, writes, false);
    collect_var_uses(ifexpr->then_expr.get(), reads, writes, false);
    collect_var_uses(ifexpr->else_expr.get(), reads, writes, false);
    return;
  }
  if (auto *mcall = dynamic_cast<MethodCallExpr *>(expr)) {
    collect_var_uses(mcall->object.get(), reads, writes, false);
    for (auto &arg : mcall->args)
      collect_var_uses(arg.get(), reads, writes, false);
    return;
  }
  if (auto *borrow = dynamic_cast<BorrowExpr *>(expr)) {
    collect_var_uses(borrow->operand.get(), reads, writes, false);
    return;
  }
  if (auto *atm = dynamic_cast<AtomicExpr *>(expr)) {
    for (auto &arg : atm->args)
      collect_var_uses(arg.get(), reads, writes, false);
    return;
  }
}

bool NLLBorrowChecker::check_borrow_rules(CFG &cfg) {
  for (auto &node : cfg.nodes) {
    if (node.id == cfg.entry || node.id == cfg.exit) continue;
    if (!node.stmt && !node.expr) continue;

    std::set<std::string> reads, writes;
    // Use CFG's precomputed gen/kill sets for complete coverage
    reads = node.gen;
    writes = node.kill;

    // Check reads against active borrows
    for (auto &var : reads) {
      for (auto &br : borrows) {
        if (br.var_name != var) continue;
        if (node.id > br.create_node && node.id <= br.end_node) {
          if (br.is_mut) {
            set_error("cannot use '" + var +
                          "' because it is mutably borrowed here",
                      br.expr);
            return false;
          }
        }
      }
    }

    // Check writes against active borrows
    for (auto &var : writes) {
      for (auto &br : borrows) {
        if (br.var_name != var) continue;
        if (node.id > br.create_node && node.id <= br.end_node) {
          std::string borrow_kind = br.is_mut ? "mutably borrowed" : "borrowed";
          set_error("cannot assign to '" + var + "' because it is " + borrow_kind + " here", br.expr);
          return false;
        }
      }
    }
  }

  // Check for overlapping mutable borrows
  for (size_t i = 0; i < borrows.size(); i++) {
    for (size_t j = i + 1; j < borrows.size(); j++) {
      auto &a = borrows[i];
      auto &b = borrows[j];
      if (a.var_name != b.var_name) continue;
      int a_start = a.create_node + 1;
      int a_end = a.end_node;
      int b_start = b.create_node + 1;
      int b_end = b.end_node;
      if (a_start <= b_end && b_start <= a_end) {
        if (a.is_mut || b.is_mut) {
          std::string a_kind = a.is_mut ? "mutably" : "shared";
          std::string b_kind = b.is_mut ? "mutably" : "shared";
          set_error("cannot borrow '" + a.var_name + "' as " + b_kind +
                        " because it is already borrowed as " + a_kind,
                    a.is_mut ? a.expr : b.expr);
          return false;
        }
      }
    }
  }

  return true;
}

// =========================================================================
// Return-of-borrow checking
//
// When a function returns &T or &mut T, the returned reference borrows
// from one of its arguments. We check that:
//   1. The return expression is a BorrowExpr or a variable holding a reference
//   2. The borrowed variable is still live at the return point
//   3. The returned reference doesn't outlive its source
//
// Since we don't track lifetime parameters, we use a simple rule:
//   - If the return type is &T or &mut T, the return expression must be
//     a BorrowExpr (not just any value)
//   - The borrowed variable must be a function parameter (not a local)
//     since locals don't survive the function exit
// =========================================================================

bool NLLBorrowChecker::check_return_borrows(
    const std::string &fn_name, FnDecl *decl) {
  if (!is_ref_type(decl->return_type)) return true; // not returning a reference

  // Find the return statement(s) in the body
  for (auto &stmt : decl->body) {
    if (auto *ret = dynamic_cast<ReturnStmt *>(stmt.get())) {
      if (!ret->value) {
        set_error("function '" + fn_name +
                      "' returns a reference but has empty return",
                  nullptr);
        return false;
      }
      // Check that the return expression is a borrow or a parameter
      if (auto *borrow = dynamic_cast<BorrowExpr *>(ret->value.get())) {
        // return &x — check that x is a parameter
        std::string var = root_var(borrow->operand.get());
        bool is_param = false;
        for (auto &p : decl->params) {
          if (p.name == var) { is_param = true; break; }
        }
        if (!is_param) {
          set_error("cannot return reference to local variable '" + var +
                        "' — it will be dropped at function exit",
                    ret->value.get());
          return false;
        }
        // Check that the borrow is not mutable when the return type is shared
        if (!borrow->is_mut && is_mut_ref_type(decl->return_type)) {
          set_error("function returns &mut T but return expression is &" + var,
                    ret->value.get());
          return false;
        }
      } else if (auto *id = dynamic_cast<IdentExpr *>(ret->value.get())) {
        // return x — check that x is a parameter with ref type
        bool is_param = false;
        for (auto &p : decl->params) {
          if (p.name == id->name && is_ref_type(p.type_ann)) {
            is_param = true;
            break;
          }
        }
        if (!is_param) {
          set_error(
              "cannot return reference to local variable '" + id->name +
                  "' — only parameters can be returned as references",
              ret->value.get());
          return false;
        }
      } else {
        set_error(
            "function returns a reference but return expression is not "
            "a borrow or parameter reference",
            ret->value.get());
        return false;
      }
    }
  }
  return true;
}

// =========================================================================
// Public API
// =========================================================================

bool NLLBorrowChecker::check_body(
    const std::string &name,
    const std::vector<std::unique_ptr<Stmt>> &body) {
  has_error_ = false;
  error_msg_.clear();
  borrows.clear();

  // Step 1: Build CFG
  CFG cfg;
  CFGBuilder builder(cfg);
  builder.build(body);

  // Step 2: Compute liveness
  cfg.compute_liveness();

  // Step 3: Walk CFG nodes to find borrows
  for (auto &node : cfg.nodes) {
    if (node.id == cfg.entry || node.id == cfg.exit) continue;
    if (node.stmt)
      collect_borrows(node.stmt, cfg, node.id);
    if (node.expr)
      collect_borrows_expr(node.expr, node.id, "");
  }

  // Step 3b: Check closure bodies
  for (auto &stmt : body) {
    if (check_closures_in_stmt(stmt.get())) {
      if (has_error_) return false;
    }
  }

  // Step 4: Compute borrow lifetimes
  compute_borrow_lifetimes(cfg);

  // Step 5: Check rules
  if (!check_borrow_rules(cfg)) {
    std::cerr << error_msg_ << "\n";
    return false;
  }

  return true;
}

bool NLLBorrowChecker::check_fn(const std::string &fn_name, FnDecl *decl) {
  // First check return-of-borrow validity
  if (!check_return_borrows(fn_name, decl)) {
    std::cerr << error_msg_ << "\n";
    return false;
  }
  return check_body(fn_name, decl->body);
}

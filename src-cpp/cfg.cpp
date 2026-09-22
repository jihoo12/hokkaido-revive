#include "cfg.h"

// =========================================================================
// CFG construction
// =========================================================================

int CFG::add_node(Stmt *stmt, Expr *expr) {
  int id = (int)nodes.size();
  nodes.push_back({id, stmt, expr});
  return id;
}

void CFG::add_edge(int from, int to) {
  if (from < 0 || to < 0) return;
  nodes[from].successors.push_back(to);
  nodes[to].predecessors.push_back(from);
}

// =========================================================================
// Liveness analysis (iterative dataflow)
// =========================================================================

void CFG::compute_liveness() {
  int n = (int)nodes.size();
  live_in.resize(n);
  live_out.resize(n);

  // Compute gen/kill for each node
  for (auto &node : nodes) {
    // gen and kill already set during CFG construction
  }

  // Iterative dataflow: live_in[B] = gen[B] ∪ (live_out[B] \ kill[B])
  //                      live_out[B] = ∪ live_in[S] for all successors S
  bool changed = true;
  while (changed) {
    changed = false;
    // Iterate in reverse order (approximates reverse post-order)
    for (int i = n - 1; i >= 0; --i) {
      auto &node = nodes[i];

      // live_out[B] = union of live_in of all successors
      std::set<std::string> new_out;
      for (int succ : node.successors) {
        for (auto &v : live_in[succ])
          new_out.insert(v);
      }

      // live_in[B] = gen[B] ∪ (live_out[B] \ kill[B])
      std::set<std::string> new_in = node.gen;
      for (auto &v : new_out) {
        if (node.kill.find(v) == node.kill.end())
          new_in.insert(v);
      }

      if (new_in != live_in[i] || new_out != live_out[i]) {
        live_in[i] = std::move(new_in);
        live_out[i] = std::move(new_out);
        changed = true;
      }
    }
  }
}

std::vector<int> CFG::find_uses(const std::string &var) const {
  std::vector<int> result;
  for (auto &node : nodes) {
    if (node.gen.count(var))
      result.push_back(node.id);
  }
  return result;
}

int CFG::last_live_point(const std::string &var) const {
  int last = -1;
  for (auto &node : nodes) {
    if (node.gen.count(var) || node.kill.count(var) ||
        live_out[node.id].count(var)) {
      last = node.id;
    }
  }
  return last;
}

// =========================================================================
// Expression gen/kill collection
// =========================================================================

void CFGBuilder::collect_expr_gen_kill(Expr *expr, std::set<std::string> &gen,
                                       std::set<std::string> &kill,
                                       bool is_lvalue) {
  if (!expr) return;

  if (auto *id = dynamic_cast<IdentExpr *>(expr)) {
    if (is_lvalue)
      kill.insert(id->name);
    else
      gen.insert(id->name);
    return;
  }
  if (auto *bin = dynamic_cast<BinaryExpr *>(expr)) {
    collect_expr_gen_kill(bin->left.get(), gen, kill, false);
    collect_expr_gen_kill(bin->right.get(), gen, kill, false);
    return;
  }
  if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
    collect_expr_gen_kill(unary->operand.get(), gen, kill, false);
    return;
  }
  if (auto *call = dynamic_cast<CallExpr *>(expr)) {
    collect_expr_gen_kill(call->callee_expr.get(), gen, kill, false);
    for (auto &arg : call->args)
      collect_expr_gen_kill(arg.get(), gen, kill, false);
    return;
  }
  if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
    collect_expr_gen_kill(assign->value.get(), gen, kill, false);
    collect_expr_gen_kill(assign->target.get(), gen, kill, true);
    return;
  }
  if (auto *compound = dynamic_cast<CompoundAssignExpr *>(expr)) {
    collect_expr_gen_kill(compound->value.get(), gen, kill, false);
    collect_expr_gen_kill(compound->target.get(), gen, kill, true);
    // Compound assign reads the target too
    collect_expr_gen_kill(compound->target.get(), gen, kill, false);
    return;
  }
  if (auto *borrow = dynamic_cast<BorrowExpr *>(expr)) {
    collect_expr_gen_kill(borrow->operand.get(), gen, kill, false);
    return;
  }
  if (auto *deref = dynamic_cast<DerefExpr *>(expr)) {
    collect_expr_gen_kill(deref->operand.get(), gen, kill, false);
    return;
  }
  if (auto *sub = dynamic_cast<SubscriptExpr *>(expr)) {
    collect_expr_gen_kill(sub->array.get(), gen, kill, false);
    collect_expr_gen_kill(sub->index.get(), gen, kill, false);
    return;
  }
  if (auto *field = dynamic_cast<FieldAccessExpr *>(expr)) {
    collect_expr_gen_kill(field->object.get(), gen, kill, is_lvalue);
    return;
  }
  if (auto *arr = dynamic_cast<ArrayLitExpr *>(expr)) {
    for (auto &el : arr->elements)
      collect_expr_gen_kill(el.get(), gen, kill, false);
    return;
  }
  if (auto *tup = dynamic_cast<TupleExpr *>(expr)) {
    for (auto &el : tup->elements)
      collect_expr_gen_kill(el.get(), gen, kill, false);
    return;
  }
  if (auto *ctor = dynamic_cast<ConstructorExpr *>(expr)) {
    for (auto &[_, fexpr] : ctor->fields)
      collect_expr_gen_kill(fexpr.get(), gen, kill, false);
    return;
  }
  if (auto *match = dynamic_cast<MatchExpr *>(expr)) {
    collect_expr_gen_kill(match->value.get(), gen, kill, false);
    for (auto &arm : match->arms)
      collect_expr_gen_kill(arm.expr.get(), gen, kill, false);
    return;
  }
  if (auto *ifexpr = dynamic_cast<IfExpr *>(expr)) {
    collect_expr_gen_kill(ifexpr->condition.get(), gen, kill, false);
    collect_expr_gen_kill(ifexpr->then_expr.get(), gen, kill, false);
    collect_expr_gen_kill(ifexpr->else_expr.get(), gen, kill, false);
    return;
  }
  if (auto *mcall = dynamic_cast<MethodCallExpr *>(expr)) {
    collect_expr_gen_kill(mcall->object.get(), gen, kill, false);
    for (auto &arg : mcall->args)
      collect_expr_gen_kill(arg.get(), gen, kill, false);
    return;
  }
  // Literals, null, asm, atomic — no variable references
}

void CFGBuilder::collect_stmt_gen_kill(Stmt *stmt, std::set<std::string> &gen,
                                       std::set<std::string> &kill) {
  if (!stmt) return;

  if (auto *expr_s = dynamic_cast<ExprStmt *>(stmt)) {
    collect_expr_gen_kill(expr_s->expr.get(), gen, kill, false);
    return;
  }
  if (auto *let = dynamic_cast<LetStmt *>(stmt)) {
    if (let->init_expr)
      collect_expr_gen_kill(let->init_expr.get(), gen, kill, false);
    kill.insert(let->name);
    return;
  }
  if (auto *ret = dynamic_cast<ReturnStmt *>(stmt)) {
    if (ret->value)
      collect_expr_gen_kill(ret->value.get(), gen, kill, false);
    return;
  }
  // break, continue, region — no variable references in gen/kill
}

// =========================================================================
// CFG builder — statement-level
// =========================================================================

int CFGBuilder::build_stmts(const std::vector<std::unique_ptr<Stmt>> &stmts,
                            int entry) {
  int cur = entry;
  for (auto &stmt : stmts) {
    cur = build_stmt(stmt.get(), cur);
    if (cur < 0) break; // terminated (return/break/continue)
  }
  return cur;
}

int CFGBuilder::build_stmt(Stmt *stmt, int entry) {
  if (!stmt) return entry;

  if (auto *ifs = dynamic_cast<IfStmt *>(stmt)) {
    // Entry node: evaluate condition
    int cond_node = cfg.add_node(stmt);
    cfg.add_edge(entry, cond_node);

    // Gen from condition
    std::set<std::string> cond_gen, cond_kill;
    collect_expr_gen_kill(ifs->condition.get(), cond_gen, cond_kill);
    cfg.nodes[cond_node].gen = cond_gen;
    cfg.nodes[cond_node].kill = cond_kill;

    // Then branch
    int then_entry = cfg.add_node();
    cfg.add_edge(cond_node, then_entry);
    int then_exit = build_stmts(ifs->then_branch, then_entry);

    // Else branch
    int else_entry = cfg.add_node();
    cfg.add_edge(cond_node, else_entry);
    int else_exit = build_stmts(ifs->else_branch, else_entry);

    // Merge node
    int merge = cfg.add_node();
    if (then_exit >= 0) cfg.add_edge(then_exit, merge);
    if (else_exit >= 0) cfg.add_edge(else_exit, merge);

    return merge;
  }

  if (auto *for_s = dynamic_cast<ForStmt *>(stmt)) {
    // Init
    int cur = entry;
    if (for_s->init) {
      int init_node = cfg.add_node(for_s->init.get());
      cfg.add_edge(cur, init_node);
      std::set<std::string> g, k;
      collect_stmt_gen_kill(for_s->init.get(), g, k);
      cfg.nodes[init_node].gen = g;
      cfg.nodes[init_node].kill = k;
      cur = init_node;
    }

    // Condition
    int cond_node = cfg.add_node(stmt);
    cfg.add_edge(cur, cond_node);
    if (for_s->condition) {
      std::set<std::string> g, k;
      collect_expr_gen_kill(for_s->condition.get(), g, k);
      cfg.nodes[cond_node].gen = g;
      cfg.nodes[cond_node].kill = k;
    }

    // Body
    int body_entry = cfg.add_node();
    cfg.add_edge(cond_node, body_entry);
    int body_exit = build_stmts(for_s->body, body_entry);

    // Update (wrap expr in a synthetic statement for the CFG node)
    int update_node = -1;
    if (for_s->update) {
      update_node = cfg.add_node(nullptr, for_s->update.get());
      if (body_exit >= 0) cfg.add_edge(body_exit, update_node);
      std::set<std::string> g, k;
      collect_expr_gen_kill(for_s->update.get(), g, k);
      cfg.nodes[update_node].gen = g;
      cfg.nodes[update_node].kill = k;
    }

    // Back-edge: update (or body) → condition
    int back_src = update_node >= 0 ? update_node : body_exit;
    if (back_src >= 0) cfg.add_edge(back_src, cond_node);

    // Exit: condition falls through to merge when false
    int merge = cfg.add_node();
    cfg.add_edge(cond_node, merge);

    return merge;
  }

  if (auto *while_s = dynamic_cast<WhileStmt *>(stmt)) {
    // Condition
    int cond_node = cfg.add_node(stmt);
    cfg.add_edge(entry, cond_node);
    if (while_s->condition) {
      std::set<std::string> g, k;
      collect_expr_gen_kill(while_s->condition.get(), g, k);
      cfg.nodes[cond_node].gen = g;
      cfg.nodes[cond_node].kill = k;
    }

    // Body
    int body_entry = cfg.add_node();
    cfg.add_edge(cond_node, body_entry);
    int body_exit = build_stmts(while_s->body, body_entry);

    // Back-edge: body → condition
    if (body_exit >= 0) cfg.add_edge(body_exit, cond_node);

    // Exit: condition falls through to merge
    int merge = cfg.add_node();
    cfg.add_edge(cond_node, merge);

    return merge;
  }

  if (auto *region = dynamic_cast<RegionStmt *>(stmt)) {
    int r_entry = cfg.add_node();
    cfg.add_edge(entry, r_entry);
    int r_exit = build_stmts(region->body, r_entry);
    int r_exit_node = cfg.add_node();
    if (r_exit >= 0) cfg.add_edge(r_exit, r_exit_node);
    return r_exit_node;
  }

  // Simple statement: single node with gen/kill
  int node_id = cfg.add_node(stmt);
  cfg.add_edge(entry, node_id);
  std::set<std::string> g, k;
  collect_stmt_gen_kill(stmt, g, k);
  cfg.nodes[node_id].gen = g;
  cfg.nodes[node_id].kill = k;

  // Return/break/continue terminate the flow
  if (dynamic_cast<ReturnStmt *>(stmt) || dynamic_cast<BreakStmt *>(stmt) ||
      dynamic_cast<ContinueStmt *>(stmt)) {
    cfg.add_edge(node_id, exit_node);
    return -1; // terminated
  }

  return node_id;
}

void CFGBuilder::build(const std::vector<std::unique_ptr<Stmt>> &body) {
  // Entry node (no statement)
  cfg.entry = cfg.add_node();
  exit_node = cfg.add_node();
  cfg.exit = exit_node;

  build_stmts(body, cfg.entry);

  // Connect any unterminated fall-through to exit
  // (build_stmts returns the last live node; we connect it in the caller
  // or handle it here by scanning for nodes with no successors except exit)
  // For the function body, we just connect the last statement node to exit.
  // This is handled by the entry-level call.
}

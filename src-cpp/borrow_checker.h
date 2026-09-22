#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "cfg.h"

// =========================================================================
// Hokkaido Language — NLL Borrow Checker
// =========================================================================
//
// Non-lexical lifetime borrow checker that enforces:
//   - At any program point, a value may have either one mutable borrow (&mut T)
//     or any number of shared borrows (&T), but not both.
//   - While a value is borrowed, the original cannot be mutated (if
//     immutably borrowed) or used at all (if mutably borrowed).
//   - Borrows end at their last use point (not end of scope).
//   - Cross-function: callee parameter types drive borrow registration.
//   - Method receivers: self param type drives implicit borrow of object.
//   - Return of borrows: returned &T must not outlive function scope.

class NLLBorrowChecker {
public:
  struct BorrowRegion {
    std::string var_name;     // variable being borrowed (e.g. "a" in &a)
    std::string ref_var;      // variable holding the reference (e.g. "w1" in let w1 = &a)
                               // empty for inline borrows (e.g. foo(&a))
    bool is_mut;
    int create_node;   // CFG node where &x / &mut x is created
    int end_node;      // CFG node where the borrow last matters
    Expr *expr;        // for error reporting
  };

private:
  std::vector<BorrowRegion> borrows;
  bool has_error_ = false;
  std::string error_msg_;

  // Program-level declarations for cross-function checking
  std::map<std::string, FnDecl *> fn_decls;         // name -> FnDecl
  std::map<std::string, FnDecl *> extern_decls;     // name -> FnDecl (extern)
  std::vector<ImplDecl *> impl_decls;                // all impl blocks

  void set_error(const std::string &msg, Expr *expr = nullptr);

  // Walk AST collecting borrows and mapping each BorrowExpr to its CFG node
  void collect_borrows(Stmt *stmt, CFG &cfg, int node_id);
  void collect_borrows_expr(Expr *expr, int node_id,
                            const std::string &ref_var);

  // Check closure bodies found during AST walk
  bool check_closures_in_stmt(Stmt *stmt);
  bool check_closures_in_expr(Expr *expr);

  // After collecting borrows, compute lifetimes using liveness
  void compute_borrow_lifetimes(CFG &cfg);

  // Check all borrow rules
  bool check_borrow_rules(CFG &cfg);

  // Check return-of-borrow validity
  bool check_return_borrows(const std::string &fn_name, FnDecl *decl);

  // Cross-function helpers
  FnDecl *lookup_fn(const std::string &name);

public:
  NLLBorrowChecker() = default;

  // Pass all program declarations for cross-function checking
  void set_declarations(const std::vector<std::unique_ptr<Decl>> &decls);

  bool check_fn(const std::string &fn_name, FnDecl *decl);
  bool check_body(const std::string &name,
                  const std::vector<std::unique_ptr<Stmt>> &body);
  bool ok() const { return !has_error_; }
  const std::string &error() const { return error_msg_; }
};

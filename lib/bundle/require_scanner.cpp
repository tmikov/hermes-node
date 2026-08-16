/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/require_scanner.h>

#include <hermes/node-compat/bundle/cjs_wrapper.h>

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Sema/SemContext.h"
#include "hermes/Sema/SemResolve.h"
#include "hermes/Support/SourceErrorManager.h"

#include "llvh/Support/Casting.h"
#include "llvh/Support/SourceMgr.h"

#include <algorithm>
#include <unordered_set>

namespace hermes {
namespace node_compat {

namespace {

/// Walks the whole AST looking for uses of \p requireDecl -- the CommonJS
/// wrapper's `require` parameter, and so the module's real require.
///
/// A call of it with a literal argument contributes the literal to `*out_`
/// (deduplicated, first-seen order). Every other use is a gap recorded in
/// `*gaps_`: a call with a computed argument, or the identifier appearing
/// anywhere it is not being called and not being read through (`require.foo`
/// is not a gap -- `require.cache` and `require.resolve` load nothing).
///
/// Inherits hermes::ESTree::RecursionDepthTracker rather than the brief's
/// trivial always-true stub: RecursiveVisitor.h documents
/// incRecursionDepth()/decRecursionDepth() as the mechanism that protects
/// the traversal against a stack overflow on pathologically deep input (a
/// minified or generated file can nest expressions far deeper than
/// hand-written source), and the tracker already implements that protocol
/// correctly, so there is no reason to hand-roll a weaker version.
class RequireVisitor : public ESTree::RecursionDepthTracker<RequireVisitor> {
 public:
  RequireVisitor(
      std::vector<std::string> *out,
      std::vector<RequireGap> *gaps,
      SourceErrorManager *sm,
      sema::SemContext *semCtx,
      sema::Decl *requireDecl,
      ESTree::IdentifierNode *requireParam)
      : out_(out),
        gaps_(gaps),
        sm_(sm),
        semCtx_(semCtx),
        requireDecl_(requireDecl),
        accounted_{requireParam} {}

  /// Called by RecursionDepthTracker once the nesting limit is hit. Once
  /// this fires, RecursionDepthTracker::incRecursionDepth() keeps returning
  /// false for the rest of the traversal (see RecursiveVisitor.h), so the
  /// walk effectively stops; scanRequires() turns overflowed() into a
  /// reported error rather than silently returning a partial result.
  void recursionDepthExceeded(ESTree::Node *) {
    overflowed_ = true;
  }

  bool overflowed() const {
    return overflowed_;
  }

  void visit(ESTree::Node *node) {
    ESTree::visitESTreeChildren(*this, node);
  }

  void visit(ESTree::CallExpressionNode *node) {
    collect(node);
    ESTree::visitESTreeChildren(*this, node);
  }

  // `require?.('x')` parses as OptionalCallExpressionNode, a distinct
  // ESTree kind (sibling of CallExpressionNode under CallExpressionLike in
  // ESTree.def) that CallExpressionNode's overload above never sees. It
  // shares the same _callee/_arguments shape, so collect() (templated
  // below) applies unchanged.
  void visit(ESTree::OptionalCallExpressionNode *node) {
    collect(node);
    ESTree::visitESTreeChildren(*this, node);
  }

  // `require.resolve`, `require.cache`, `require.main`: reading a property
  // of require loads nothing, so the identifier is accounted for and is not
  // an escape. Both member-expression kinds, for the same reason both call
  // kinds are handled: `require?.resolve` is a distinct ESTree node.
  void visit(ESTree::MemberExpressionNode *node) {
    accountFor(node->_object);
    ESTree::visitESTreeChildren(*this, node);
  }

  void visit(ESTree::OptionalMemberExpressionNode *node) {
    accountFor(node->_object);
    ESTree::visitESTreeChildren(*this, node);
  }

  // Every remaining appearance of the identifier. Reached after the visits
  // above have run on the enclosing node -- RecursiveVisitor descends into
  // children only once the parent's overload has, so an identifier that is
  // a callee or a member object has already been accounted for by the time
  // this sees it.
  void visit(ESTree::IdentifierNode *node) {
    if (isRequire(node) && accounted_.count(node) == 0)
      record(RequireGapKind::kEscapedValue, node);
    ESTree::visitESTreeChildren(*this, node);
  }

 private:
  /// \return true if \p node is an identifier bound to the module's
  /// `require` parameter.
  bool isRequire(ESTree::Node *node) const {
    auto *ident = llvh::dyn_cast<ESTree::IdentifierNode>(node);
    // An identifier the resolver gave up on carries no decl to compare, and
    // getExpressionDecl() asserts rather than tolerating one.
    if (ident == nullptr || ident->isUnresolvable())
      return false;
    return semCtx_->getExpressionDecl(ident) == requireDecl_;
  }

  /// Marks \p node as a use of `require` that is not an escape, so the
  /// IdentifierNode overload does not report it when the walk reaches it.
  void accountFor(ESTree::Node *node) {
    if (isRequire(node))
      accounted_.insert(node);
  }
  /// Appends the require() argument of \p node to `*out_` if \p node is a
  /// call to a bare `require` identifier with a literal first argument that
  /// has not already been collected. See require_scanner.h for the exact
  /// acceptance rules.
  ///
  /// Templated so CallExpressionNode and OptionalCallExpressionNode -- two
  /// distinct ESTree node kinds with identical `_callee`/`_arguments`
  /// fields -- share this one implementation instead of duplicating the
  /// callee/argument checks per kind.
  template <typename CallNodeT>
  void collect(CallNodeT *node) {
    if (!isRequire(node->_callee))
      return;
    // Being called is what a require is for: whatever the argument turns
    // out to be, this use is not an escape.
    accounted_.insert(node->_callee);

    if (node->_arguments.empty())
      return;
    ESTree::Node &firstArg = node->_arguments.front();

    std::string value;
    if (auto *str = llvh::dyn_cast<ESTree::StringLiteralNode>(&firstArg)) {
      value = str->_value->str().str();
    } else if (
        auto *tmpl = llvh::dyn_cast<ESTree::TemplateLiteralNode>(&firstArg)) {
      // A template literal is a literal only when it has no substitutions.
      // The grammar guarantees quasis.size() == expressions.size() + 1, so
      // "no substitutions" is exactly "exactly one quasi".
      if (!tmpl->_expressions.empty() || tmpl->_quasis.empty()) {
        record(RequireGapKind::kComputedArgument, node);
        return;
      }
      auto *elem =
          llvh::cast<ESTree::TemplateElementNode>(&tmpl->_quasis.front());
      // _cooked is null when the quasi has an invalid escape; _raw is
      // always present and, with no escapes possible in an unterminated
      // literal already having failed to parse, matches cooked whenever
      // cooked is available.
      UniqueString *text =
          elem->_cooked != nullptr ? elem->_cooked : elem->_raw;
      value = text->str().str();
    } else {
      record(RequireGapKind::kComputedArgument, node);
      return;
    }

    if (std::find(out_->begin(), out_->end(), value) == out_->end())
      out_->push_back(std::move(value));
  }

  /// Records \p node's position as a use of require this scan could not
  /// follow. Not deduplicated: two such uses are two holes in the container
  /// even when they would compute the same string, and the position is the
  /// whole value of the record.
  ///
  /// The coordinates are converted back out of the CommonJS wrapper, so a
  /// caller never has to know the scan added one.
  void record(RequireGapKind kind, ESTree::Node *node) {
    if (gaps_ == nullptr)
      return;
    SourceErrorManager::SourceCoords coords;
    if (!sm_->findBufferLineAndLoc(node->getStartLoc(), coords))
      return;
    RequireGap gap;
    gap.kind = kind;
    gap.line = static_cast<uint32_t>(coords.line);
    gap.column = static_cast<uint32_t>(coords.col);
    unwrapCoords(gap.line, &gap.column);
    gaps_->push_back(gap);
  }

  std::vector<std::string> *out_;
  std::vector<RequireGap> *gaps_;
  SourceErrorManager *sm_;
  sema::SemContext *semCtx_;
  sema::Decl *requireDecl_;
  /// Uses of `require` that are already explained -- a callee, or the object
  /// of a member expression -- so the IdentifierNode overload can report
  /// everything else without needing a parent pointer.
  std::unordered_set<ESTree::Node *> accounted_;
  bool overflowed_ = false;
};

/// Appends every DK_Error diagnostic to \p ctx (a std::string*) instead of
/// letting SourceErrorManager's default handler print it to stderr.
/// Installed before parsing so a parse error is captured into
/// scanRequires()'s `error` output, not spewed to the terminal to be found
/// later by whatever consumes the AOT bundle producer's own
/// stdout/stderr.
///
/// Non-error kinds (DK_Warning, DK_Note, DK_Remark) are dropped rather than
/// appended. SourceErrorManager enables warning categories by default --
/// e.g. a legacy-octal-looking numeric literal like `019` -- and those are
/// exactly the kind of un-curated node_modules source this scanner runs
/// over at scale. A caller reasonably treats a non-empty `error` as "the
/// scan failed"; leaking warning text into it on a successful scan would
/// make that check wrong.
void diagHandler(const llvh::SMDiagnostic &diag, void *ctx) {
  if (diag.getKind() != llvh::SourceMgr::DK_Error)
    return;
  auto *error = static_cast<std::string *>(ctx);
  if (!error->empty())
    error->push_back('\n');
  if (diag.getLineNo() > 0) {
    *error += std::to_string(diag.getLineNo());
    error->push_back(':');
    *error += std::to_string(diag.getColumnNo());
    *error += ": ";
  }
  *error += diag.getMessage().str();
}

/// \return the FunctionExpressionNode of the CommonJS wrapper wrapCJS() put
/// around the source, or null if \p program is not that shape.
///
/// The wrapper is `(function(...) { ... })` -- a Program whose single
/// statement is an expression statement holding a parenthesized function
/// expression. Nothing about the module's own text can change that, since
/// the module's text is entirely inside the function body.
ESTree::FunctionExpressionNode *findWrapper(ESTree::ProgramNode *program) {
  if (program->_body.size() != 1)
    return nullptr;
  auto *stmt =
      llvh::dyn_cast<ESTree::ExpressionStatementNode>(&program->_body.front());
  if (stmt == nullptr)
    return nullptr;
  return llvh::dyn_cast<ESTree::FunctionExpressionNode>(stmt->_expression);
}

/// \return \p wrapper's parameter named \p name, or null.
ESTree::IdentifierNode *findParam(
    ESTree::FunctionExpressionNode *wrapper,
    llvh::StringRef name) {
  for (ESTree::Node &param : wrapper->_params) {
    auto *ident = llvh::dyn_cast<ESTree::IdentifierNode>(&param);
    if (ident != nullptr && ident->_name->str() == name)
      return ident;
  }
  return nullptr;
}

} // namespace

bool scanRequires(
    std::string_view source,
    bool enableTS,
    std::vector<std::string> *out,
    std::string *error,
    std::vector<RequireGap> *gaps) {
  error->clear();

  // Wrapped, so this scan parses and resolves exactly the text the compiler
  // will compile: a module body is a function body, and reading it as a
  // Program rejects a top-level `return` that runs perfectly well from
  // disk. It is also what gives `require` a binding to resolve against.
  // See cjs_wrapper.h.
  //
  // wrapCJS() also settles the nul-termination question JSParser's StringRef
  // constructor raises: it hands the bytes to
  // llvh::MemoryBuffer::getMemBuffer() with RequiresNullTerminator = true --
  // the lexer reads one byte past the logical end as an EOF sentinel, and
  // MemoryBuffer only asserts (rather than checks) that it is '\0'.
  // std::string_view carries no such guarantee, since a caller could pass a
  // view into the middle of a larger buffer, but the owned std::string built
  // here does.
  std::string ownedSource = wrapCJS(source);

  auto context = std::make_shared<Context>();
  if (enableTS)
    context->setParseTS(true);
  context->getSourceErrorManager().setDiagHandler(&diagHandler, error);

  parser::JSParser parser(
      *context, llvh::StringRef(ownedSource.data(), ownedSource.size()));
  llvh::Optional<ESTree::ProgramNode *> program = parser.parse();
  if (!program) {
    if (error->empty())
      *error = "hermes-node bundle: parse error";
    return false;
  }

  // Resolution is what turns "an identifier spelled require" into "the
  // module's require". It reports through the same diag handler, so a
  // failure lands in *error like a parse error does -- and a failure here
  // means the compiler will reject the same wrapped text for the same
  // reason, so treating it as a scan failure is not a new restriction.
  sema::SemContext semCtx(*context);
  if (!sema::resolveAST(*context, semCtx, *program)) {
    if (error->empty())
      *error = "hermes-node bundle: semantic resolution error";
    return false;
  }

  ESTree::FunctionExpressionNode *wrapper = findWrapper(*program);
  ESTree::IdentifierNode *requireParam =
      wrapper != nullptr ? findParam(wrapper, "require") : nullptr;
  sema::Decl *requireDecl = requireParam != nullptr
      ? semCtx.getDeclarationDecl(requireParam)
      : nullptr;
  if (requireDecl == nullptr) {
    // Unreachable barring a bug in wrapCJS() or in this file's idea of the
    // wrapper's shape. Reported rather than asserted because silently
    // scanning nothing would look exactly like a module with no
    // dependencies, and the bundle would be quietly wrong.
    *error = "hermes-node bundle: internal: no require binding in the wrapper";
    return false;
  }

  // The parameter that declares `require` is a declaration, not a use of
  // it, and Hermes records an expression decl on it like any other
  // identifier -- so without this the wrapper's own parameter is reported
  // as an escape in every single module.
  RequireVisitor visitor(
      out,
      gaps,
      &context->getSourceErrorManager(),
      &semCtx,
      requireDecl,
      requireParam);
  ESTree::visitESTreeNodeNoReplace(visitor, *program);
  if (visitor.overflowed()) {
    *error = "hermes-node bundle: source is too deeply nested to scan";
    return false;
  }
  return true;
}

} // namespace node_compat
} // namespace hermes

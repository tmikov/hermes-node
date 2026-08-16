/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/require_scanner.h>

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Support/SourceErrorManager.h"

#include "llvh/Support/Casting.h"
#include "llvh/Support/SourceMgr.h"

#include <algorithm>

namespace hermes {
namespace node_compat {

namespace {

/// Walks the whole AST looking for calls of the shape `require(<literal>)`,
/// collecting the literal into `*out_` (deduplicated, first-seen order), and
/// the position of every other `require()` call into `*computed_`.
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
      std::vector<ComputedRequire> *computed,
      SourceErrorManager *sm)
      : out_(out), computed_(computed), sm_(sm) {}

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

 private:
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
    auto *callee = llvh::dyn_cast<ESTree::IdentifierNode>(node->_callee);
    if (callee == nullptr || callee->_name->str() != "require")
      return;

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
        recordComputed(node);
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
      recordComputed(node);
      return;
    }

    if (std::find(out_->begin(), out_->end(), value) == out_->end())
      out_->push_back(std::move(value));
  }

  /// Records the position of \p node as a require() this scan could not
  /// follow. Not deduplicated: two computed require() calls are two holes in
  /// the container even when they compute the same string, and the position
  /// is the whole value of the record.
  template <typename CallNodeT>
  void recordComputed(CallNodeT *node) {
    if (computed_ == nullptr)
      return;
    SourceErrorManager::SourceCoords coords;
    if (!sm_->findBufferLineAndLoc(node->getStartLoc(), coords))
      return;
    computed_->push_back(
        {static_cast<uint32_t>(coords.line),
         static_cast<uint32_t>(coords.col)});
  }

  std::vector<std::string> *out_;
  std::vector<ComputedRequire> *computed_;
  SourceErrorManager *sm_;
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

} // namespace

bool scanRequires(
    std::string_view source,
    bool enableTS,
    std::vector<std::string> *out,
    std::string *error,
    std::vector<ComputedRequire> *computed) {
  error->clear();

  // JSParser's StringRef constructor hands the bytes to
  // llvh::MemoryBuffer::getMemBuffer() with RequiresNullTerminator = true:
  // the lexer reads one byte past the logical end of the buffer as an EOF
  // sentinel, and MemoryBuffer only asserts (rather than checks) that it is
  // '\0'. std::string_view carries no such guarantee -- a caller could pass
  // a view into the middle of a larger buffer -- so the source is copied
  // into an owned std::string, whose data() is always nul-terminated, and
  // that owned copy (not the caller's view) backs the parser for the rest
  // of this function.
  std::string ownedSource(source);

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

  RequireVisitor visitor(out, computed, &context->getSourceErrorManager());
  ESTree::visitESTreeNodeNoReplace(visitor, *program);
  if (visitor.overflowed()) {
    *error = "hermes-node bundle: source is too deeply nested to scan";
    return false;
  }
  return true;
}

} // namespace node_compat
} // namespace hermes

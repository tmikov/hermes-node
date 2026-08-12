/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_TOOLS_HERMESPARSERNATIVE_KINDHASH_H
#define HERMES_TOOLS_HERMESPARSERNATIVE_KINDHASH_H

#include <cstdint>

namespace hermes {

/// The ordered list of node-kind entries, derived from ESTree.def.
///
/// Index \c i corresponds to \c NodeKind \c i, which is the same indexing the
/// generated JavaScript NODE_DESERIALIZERS array uses. The wire format emits
/// <tt>kind + 1</tt> so that zero can mean "null node".
///
/// A node entry is its name followed by its field names in declaration order,
/// e.g. <tt>Identifier(name,typeAnnotation,optional)</tt>. The field shape is
/// part of the entry because the serializer writes a node's fields
/// positionally, with no per-field tag: adding, removing, renaming or
/// reordering a field on an *existing* node leaves the list of node names
/// untouched while shifting every subsequent word of that node's stream. A
/// name-only hash cannot see that, and the result is a plausible-looking but
/// wrong AST rather than a clean mismatch error.
///
/// The field type and optional-flag tokens are deliberately dropped: neither
/// affects the wire layout, and including them would make the hash churn on
/// changes that cannot break the consumer.
///
/// \c ESTREE_FIRST and \c ESTREE_LAST are range markers rather than nodes, so
/// they contribute a bare name with no parenthesized field list.
// clang-format off
// One field triple per macro parameter would explode these lists to thirty
// lines each; ESTree.def itself is formatted by hand for the same reason.
static const char *const kNodeKindEntries[] = {
#define ESTREE_NODE_0_ARGS(NAME, BASE) #NAME "()",
#define ESTREE_NODE_1_ARGS(NAME, BASE, T0, N0, O0) #NAME "(" #N0 ")",
#define ESTREE_NODE_2_ARGS(NAME, BASE, T0, N0, O0, T1, N1, O1) \
  #NAME "(" #N0 "," #N1 ")",
#define ESTREE_NODE_3_ARGS(                                      \
    NAME, BASE, T0, N0, O0, T1, N1, O1, T2, N2, O2)              \
  #NAME "(" #N0 "," #N1 "," #N2 ")",
#define ESTREE_NODE_4_ARGS(                                      \
    NAME, BASE, T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3)  \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 ")",
#define ESTREE_NODE_5_ARGS(                                      \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4)  \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 ")",
#define ESTREE_NODE_6_ARGS(                                      \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4,  \
    T5, N5, O5)                                                  \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 "," #N5 ")",
#define ESTREE_NODE_7_ARGS(                                      \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4,  \
    T5, N5, O5, T6, N6, O6)                                      \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 "," #N5 "," #N6 ")",
#define ESTREE_NODE_8_ARGS(                                      \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4,  \
    T5, N5, O5, T6, N6, O6, T7, N7, O7)                          \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 "," #N5 "," #N6  \
        "," #N7 ")",
#define ESTREE_NODE_9_ARGS(                                      \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4,  \
    T5, N5, O5, T6, N6, O6, T7, N7, O7, T8, N8, O8)              \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 "," #N5 "," #N6  \
        "," #N7 "," #N8 ")",
#define ESTREE_NODE_10_ARGS(                                     \
    NAME,                                                        \
    BASE,                                                        \
    T0, N0, O0, T1, N1, O1, T2, N2, O2, T3, N3, O3, T4, N4, O4,  \
    T5, N5, O5, T6, N6, O6, T7, N7, O7, T8, N8, O8, T9, N9, O9)  \
  #NAME "(" #N0 "," #N1 "," #N2 "," #N3 "," #N4 "," #N5 "," #N6  \
        "," #N7 "," #N8 "," #N9 ")",
#define ESTREE_FIRST(NAME, ...) #NAME "First",
#define ESTREE_LAST(NAME) #NAME "Last",
#include "hermes/AST/ESTree.def"
};
// clang-format on

/// \return an FNV-1a hash over every entry of \c kNodeKindEntries, each
/// followed by a newline. Any insertion, removal or reordering of node kinds
/// changes the result, as does any change to an existing node's field list.
/// That is what lets the JavaScript side detect that it was generated from a
/// different ESTree.def than the addon was built from.
///
/// tools/hermes-parser/js/scripts/genKindHash.js computes the same value by
/// parsing ESTree.def directly. The two implementations are deliberately
/// independent: they agree only if both really see the same definitions.
///
/// This walks every entry on every call. The result is a constant of the
/// build, so callers on a hot path must use \c kindHash() instead; this
/// function exists to be the definition of that constant, and for the tests
/// which check that recomputing it is stable.
inline uint32_t computeKindHash() {
  uint32_t hash = 0x811C9DC5u;
  const auto feedByte = [&hash](unsigned char c) {
    hash ^= (uint32_t)c;
    hash *= 16777619u;
  };

  for (const char *entry : kNodeKindEntries) {
    for (const char *p = entry; *p != '\0'; ++p) {
      feedByte((unsigned char)*p);
    }
    feedByte((unsigned char)'\n');
  }

  return hash;
}

/// \return the same value as \c computeKindHash(), computed once per process.
/// The hash depends only on ESTree.def as it was compiled in, so hashing all
/// ~300 entries again on every parse() call is pure waste. Initialization of
/// the local static is thread-safe, and every thread observes the same value.
inline uint32_t kindHash() {
  static const uint32_t hash = computeKindHash();
  return hash;
}

} // namespace hermes

#endif

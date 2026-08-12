/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

'use strict';

const fs = require('fs');
const path = require('path');

const OUTPUT_FILE = path.resolve(
  __dirname,
  // Adapted from the fork's tools/hermes-parser/js/scripts/genKindHash.js,
  // which writes into a sibling directory named hermes-parser-native/. Here
  // the sibling of scripts/ is named package/ instead.
  '../package/src/HermesParserKindHash.js',
);

/**
 * FNV-1a over each entry followed by a newline. Must stay identical to
 * computeKindHash() in tools/hermes-parser-native/KindHash.h.
 */
function fnv1a(entries) {
  let hash = 0x811c9dc5;
  const feed = byte => {
    hash ^= byte;
    hash = Math.imul(hash, 16777619) >>> 0;
  };
  for (const entry of entries) {
    for (let i = 0; i < entry.length; i++) {
      feed(entry.charCodeAt(i) & 0xff);
    }
    feed(0x0a);
  }
  return hash >>> 0;
}

/**
 * Remove block and line comments. ESTree.def has no string or character
 * literals outside of comments, so this is safe to do textually, and it must
 * happen before the macro arguments are split: several field lists carry
 * trailing `// ...` notes that would otherwise be parsed as arguments.
 */
function stripComments(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
}

/**
 * Starting at `open`, the index of a `(`, return the index of the matching
 * `)`. ESTree.def never nests parentheses inside a macro invocation, but
 * tracking depth keeps this honest if that ever changes.
 */
function findMatchingParen(text, open) {
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    if (text[i] === '(') {
      depth++;
    } else if (text[i] === ')') {
      depth--;
      if (depth === 0) {
        return i;
      }
    }
  }
  throw new Error(`unterminated macro argument list at offset ${open}`);
}

/**
 * Extract the ordered node-kind entries from ESTree.def.
 *
 * An ESTREE_NODE_n_ARGS node contributes `Name(field0,field1,...)`, taking
 * the field names from every (type, name, optional) triple after the node
 * name and its base. ESTREE_FIRST(X) and ESTREE_LAST(X) are range markers,
 * not nodes, and contribute `XFirst` and `XLast`.
 *
 * The field names are part of the entry because the serializer writes fields
 * positionally: a change to an existing node's field list keeps the set of
 * node names identical while shifting the rest of that node's wire stream.
 *
 * This ordering matches NODE_DESERIALIZERS, and the resulting strings must
 * match kNodeKindEntries in tools/hermes-parser-native/KindHash.h exactly.
 */
function extractEntries(defPath) {
  const text = stripComments(fs.readFileSync(defPath, 'utf8'));
  const re = /ESTREE_(NODE_(\d+)_ARGS|FIRST|LAST)\s*\(/g;
  const entries = [];
  let m;
  while ((m = re.exec(text)) !== null) {
    const open = re.lastIndex - 1;
    const close = findMatchingParen(text, open);
    const args = text
      .slice(open + 1, close)
      .split(',')
      .map(arg => arg.trim());
    re.lastIndex = close + 1;

    // Skip the macro definitions at the top of the file, which use the
    // literal parameter name NAME rather than a real node name.
    if (args[0] === 'NAME') {
      continue;
    }

    if (m[1] === 'FIRST') {
      entries.push(args[0] + 'First');
      continue;
    }
    if (m[1] === 'LAST') {
      entries.push(args[0] + 'Last');
      continue;
    }

    const argCount = Number(m[2]);
    const expected = 2 + 3 * argCount;
    if (args.length !== expected) {
      throw new Error(
        `${args[0]}: ESTREE_NODE_${argCount}_ARGS should have ${expected} ` +
          `macro arguments but has ${args.length}`,
      );
    }
    const fields = [];
    for (let i = 0; i < argCount; i++) {
      fields.push(args[2 + i * 3 + 1]);
    }
    entries.push(`${args[0]}(${fields.join(',')})`);
  }

  if (entries.length === 0) {
    throw new Error(`no node kinds found in ${defPath}`);
  }
  return entries;
}

const includePath = process.argv[2];
if (includePath == null) {
  console.error('usage: genKindHash.js <hermes-include-path>');
  process.exit(1);
}

const entries = extractEntries(path.join(includePath, 'hermes/AST/ESTree.def'));
const hash = fnv1a(entries);

if (process.argv[3] === '--dump') {
  // Used to diff this parse against the C++ macro expansion when the two
  // hashes disagree.
  console.log(entries.join('\n'));
  process.exit(0);
}

fs.writeFileSync(
  OUTPUT_FILE,
  `/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 * @generated
 */

'use strict';

// Hash of the ${entries.length} node-kind entries in ESTree.def, in order.
// Each entry is a node name plus its field names, so this changes when a
// node is added, removed or reordered *and* when an existing node's field
// list changes.
// Must match computeKindHash() in tools/hermes-parser-native/KindHash.h.
export default ${hash};
`,
);

console.log(`genKindHash: ${entries.length} kinds, hash ${hash}`);

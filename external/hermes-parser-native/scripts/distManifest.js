/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @noflow
 * @format
 */

'use strict';

// Producer and checker for hermes-parser-native's dist build manifest.
//
// hermes-parser-native ships `main: dist/index.js`, and `dist/` is a
// gitignored build output that nothing regenerates automatically. Anything
// that loads the package by path -- the benchmarks, any consumer-shaped
// script, a copy of the package under test -- therefore runs whatever
// `dist/` happens to contain, which may predate the `src/` edit that is
// being tested. That has already happened twice in this project: a set of
// end-to-end benchmark numbers was taken against five-day-old `dist/`
// JavaScript, and a loader-precedence fix landed in `src/` sat inert
// because `dist/` still carried the old code.
//
// build-native.sh calls this file after it finishes assembling `dist/`,
// recording the SHA-256 of every file under `src/`. A jest test
// (__tests__/DistFreshness-test.js) recomputes those hashes and fails if
// they disagree, so the next stale-dist incident is a red test rather than
// a silently wrong result.
//
// Why content hashes and not mtimes: build-native.sh does `cp -r src dist`
// and then transpiles in place, so `dist/` mtimes are "now" and `src/`
// mtimes are older -- but the `rsync -a` step in the middle *preserves*
// source mtimes for the files it copies, and a `git checkout` or `git
// stash` restores content while setting mtime to now. An mtime comparison
// would need a fudge factor to survive the first and would still cry wolf
// after the second. A hash is exact in both directions: it fires when, and
// only when, the bytes `dist/` was built from are no longer the bytes in
// `src/`.

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

// Deliberately not dot-prefixed: this file has to survive `npm pack` (npm's
// handling of dotfiles inside a published directory has enough exceptions
// that relying on it is not worth the risk), and it is useful to a consumer
// debugging which sources a published `dist/` was built from.
const MANIFEST_NAME = 'build-manifest.json';
const MANIFEST_VERSION = 1;

// Printed in the failure message. Kept here so the test and the script that
// writes the manifest cannot drift from each other on what the fix is.
const REBUILD_COMMAND =
  'tools/hermes-parser/js/scripts/build-native.sh <hermes-include-dir> ' +
  '<prebuilds-dir>';

/**
 * Recursively list files under `dir`, as paths relative to `dir`, sorted.
 */
function listFiles(dir) {
  const out = [];
  const walk = (abs, rel) => {
    for (const entry of fs.readdirSync(abs, {withFileTypes: true})) {
      const childAbs = path.join(abs, entry.name);
      const childRel = rel === '' ? entry.name : `${rel}/${entry.name}`;
      if (entry.isDirectory()) {
        walk(childAbs, childRel);
      } else {
        out.push(childRel);
      }
    }
  };
  walk(dir, '');
  out.sort();
  return out;
}

function sha256(file) {
  return crypto
    .createHash('sha256')
    .update(fs.readFileSync(file))
    .digest('hex');
}

/**
 * Map every file under `srcDir` to its SHA-256. This is the single
 * definition of "what dist was built from", used both when writing the
 * manifest and when checking it, so the two cannot disagree about which
 * files count or how they are hashed.
 */
function hashSrcTree(srcDir) {
  const files = {};
  for (const rel of listFiles(srcDir)) {
    files[rel] = sha256(path.join(srcDir, rel));
  }
  return files;
}

/**
 * Provenance for the prebuilt addons `dist`'s sibling `prebuilds/`
 * directory carries. This is recorded but deliberately *not* asserted on by
 * any test -- see the note in __tests__/DistFreshness-test.js. It is here so
 * that "which binary is in this package" is answerable without running
 * anything.
 */
function describePrebuilds(prebuildsDir) {
  if (!fs.existsSync(prebuildsDir)) {
    return {};
  }
  const out = {};
  for (const rel of listFiles(prebuildsDir)) {
    const abs = path.join(prebuildsDir, rel);
    const stat = fs.statSync(abs);
    out[rel] = {
      sha256: sha256(abs),
      size: stat.size,
      mtime: stat.mtime.toISOString(),
    };
  }
  return out;
}

/**
 * Write dist/build-manifest.json for the package rooted at `packageDir`.
 * Called by build-native.sh as its last step, so a build that dies partway
 * through leaves no manifest at all -- which the checker reports as stale
 * rather than as up to date.
 */
function writeManifest(packageDir) {
  const distDir = path.join(packageDir, 'dist');
  if (!fs.existsSync(distDir)) {
    throw new Error(
      `${distDir} does not exist; nothing to write a manifest for`,
    );
  }
  const manifest = {
    manifestVersion: MANIFEST_VERSION,
    generatedAt: new Date().toISOString(),
    files: hashSrcTree(path.join(packageDir, 'src')),
    prebuilds: describePrebuilds(path.join(packageDir, 'prebuilds')),
  };
  const target = path.join(distDir, MANIFEST_NAME);
  fs.writeFileSync(target, JSON.stringify(manifest, null, 2) + '\n');
  return target;
}

/**
 * Compare the current `src/` against the manifest `dist/` was built from.
 *
 * Returns an object whose `status` is one of:
 *  - `'published'`  no `src/` directory. A published package ships `dist/`
 *                   and not `src/`, so there is nothing to be stale against
 *                   and `dist/` is authoritative by definition.
 *  - `'no-dist'`    no `dist/` directory. Nothing has been built, so nothing
 *                   stale can be loaded. Tests run from `src/` via jest's
 *                   moduleNameMapper, so this is the normal state of a fresh
 *                   checkout and must not be an error.
 *  - `'stale'`      `dist/` exists but was not built from the current
 *                   `src/`. `changed`, `added` and `removed` name the files;
 *                   `message` is ready to throw.
 *  - `'ok'`         `dist/` was built from exactly these bytes.
 */
function checkDistFreshness(packageDir) {
  const srcDir = path.join(packageDir, 'src');
  const distDir = path.join(packageDir, 'dist');
  const manifestPath = path.join(distDir, MANIFEST_NAME);

  if (!fs.existsSync(srcDir)) {
    return {status: 'published'};
  }
  if (!fs.existsSync(distDir)) {
    return {status: 'no-dist'};
  }

  const current = hashSrcTree(srcDir);

  if (!fs.existsSync(manifestPath)) {
    return {
      status: 'stale',
      changed: [],
      added: [],
      removed: [],
      message:
        `${manifestPath} is missing, so there is no record of which sources ` +
        `${distDir} was built from. Either the build predates the manifest ` +
        'or it did not run to completion. Rebuild:\n  ' +
        REBUILD_COMMAND,
    };
  }

  let manifest;
  try {
    manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  } catch (e) {
    return {
      status: 'stale',
      changed: [],
      added: [],
      removed: [],
      message:
        `${manifestPath} is not readable JSON (${e.message}). Rebuild:\n  ` +
        REBUILD_COMMAND,
    };
  }

  const recorded = manifest.files || {};
  const changed = [];
  const added = [];
  const removed = [];
  for (const rel of Object.keys(current)) {
    if (!(rel in recorded)) {
      added.push(rel);
    } else if (recorded[rel] !== current[rel]) {
      changed.push(rel);
    }
  }
  for (const rel of Object.keys(recorded)) {
    if (!(rel in current)) {
      removed.push(rel);
    }
  }

  if (changed.length === 0 && added.length === 0 && removed.length === 0) {
    return {status: 'ok'};
  }

  const lines = [
    `${distDir} is stale: it was not built from the current src/.`,
    '',
    'dist/ is a gitignored build output that nothing regenerates ' +
      'automatically, and it is what `main: dist/index.js`, the benchmarks ' +
      'and any consumer-shaped script actually load. Until it is rebuilt, ' +
      'those are running the old code.',
    '',
  ];
  const section = (label, files) => {
    if (files.length > 0) {
      lines.push(`${label}:`);
      for (const rel of files) {
        lines.push(`  src/${rel}`);
      }
      lines.push('');
    }
  };
  section('Modified since dist/ was built', changed);
  section('Added since dist/ was built', added);
  section('Removed since dist/ was built', removed);
  lines.push(`dist/ was built at ${manifest.generatedAt || '(unknown time)'}.`);
  lines.push('Rebuild it with:');
  lines.push(`  ${REBUILD_COMMAND}`);

  return {status: 'stale', changed, added, removed, message: lines.join('\n')};
}

module.exports = {
  MANIFEST_NAME,
  MANIFEST_VERSION,
  REBUILD_COMMAND,
  checkDistFreshness,
  hashSrcTree,
  writeManifest,
};

if (require.main === module) {
  const packageDir = process.argv[2];
  if (packageDir == null) {
    console.error('usage: distManifest.js <package-dir>');
    process.exit(1);
  }
  console.log(`wrote ${writeManifest(path.resolve(packageDir))}`);
}

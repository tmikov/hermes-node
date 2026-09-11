// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// esbuild driver for build-cjs.sh. Not run directly with the esbuild CLI
// because the one rewrite this build needs -- dropping a top-level await
// esbuild refuses to emit for a "cjs" output format -- needs an onLoad
// plugin, which only the JS API exposes.
//
// Two top-level awaits block a plain `esbuild --bundle --format=cjs` of
// Ink 6.4.0, both confirmed present (and, so far, the only ones) by
// running the build and reading esbuild's own errors:
//
//   1. ink/build/reconciler.js: `await import('./devtools.js')` inside
//      `if (process.env['DEV'] === 'true')` -- dead code in production.
//      Handled by the onLoad plugin below, which patches this exact file
//      and throws if the exact string it expects is not there, so a
//      version bump that changes the shape fails the build instead of
//      silently shipping something wrong.
//   2. yoga-layout's own top-level await in its default entry point --
//      handled by aliasing 'yoga-layout' to yoga-shim.mjs (see there).
//
// See ../../docs/notes/2026-08-24-ink-findings.md for the fuller story.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

const dropDevtoolsAwait = {
  name: 'drop-devtools-await',
  setup(build) {
    build.onLoad({filter: /ink[\\/]build[\\/]reconciler\.js$/}, (args) => {
      let contents = fs.readFileSync(args.path, 'utf8');
      const needle = "await import('./devtools.js')";
      if (!contents.includes(needle)) {
        throw new Error(
          `expected top-level await not found in ${args.path} -- ink's ` +
            'reconciler.js changed shape; update build.mjs (and check for ' +
            'other top-level awaits) before bundling this version'
        );
      }
      // The import stays: only the await is dropped, so this remains a
      // (fire-and-forget) dynamic import of dead code, not a rewrite of
      // what runs.
      contents = contents.replace(needle, "import('./devtools.js')");
      return {contents, loader: 'js'};
    });
  },
};

const outfile = path.join(here, 'dist-cjs', 'app.cjs');
await esbuild.build({
  entryPoints: [path.join(here, 'app.mjs')],
  bundle: true,
  platform: 'node',
  format: 'cjs',
  outfile,
  // Only reachable from the dead devtools branch above.
  external: ['react-devtools-core'],
  alias: {'yoga-layout': path.join(here, 'yoga-shim.mjs')},
  plugins: [dropDevtoolsAwait],
  logLevel: 'info',
});
console.log(`wrote ${outfile}`);

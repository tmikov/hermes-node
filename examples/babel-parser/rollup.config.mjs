import commonjs from '@rollup/plugin-commonjs';
import { nodeResolve } from '@rollup/plugin-node-resolve';
import json from '@rollup/plugin-json';
import { builtinModules } from 'node:module';

// @babel/core ships a three-line module whose only job is to call dynamic
// import(). Hermes cannot compile that, and rollup inlines everything into
// one file, so leaving it in makes the whole bundle uncompilable rather
// than one module. Replaced with a stub that throws if it is ever called --
// which is exactly what hermes-node --build-bundle does on its own.
const stubDynamicImport = {
  name: 'stub-dynamic-import',
  load(id) {
    if (id.endsWith('/@babel/core/lib/config/files/import.cjs')) {
      return 'module.exports = function import_() {\n' +
        '  throw new Error("dynamic import() is not supported");\n};\n';
    }
    return null;
  },
};

export default {
  input: 'transform-static.js',
  output: { file: 'rollup-out.cjs', format: 'cjs', exports: 'auto' },
  // 'v8' is deliberately not external: hermes-node has no such builtin,
  // and Babel only ever touches it through a lazy require inside a
  // function. Left to `ignore` below so it stays where it was written.
  external: [
    ...builtinModules.filter((m) => m !== 'v8'),
    ...builtinModules.filter((m) => m !== 'v8').map((m) => 'node:' + m),
  ],
  plugins: [
    stubDynamicImport,
    nodeResolve({ preferBuiltins: true }),
    // These are optional dependencies a package probes for inside a
    // try/catch. Without `ignore`, rollup hoists the require to the top of
    // the bundle, turning a guarded probe into an unconditional load that
    // throws before main() ever runs -- under Node as well as here.
    commonjs({
      ignoreDynamicRequires: true,
      ignore: [
        '@babel/preset-typescript',
        '@babel/preset-typescript/package.json',
        'supports-color',
        'v8',
      ],
    }),
    json(),
  ],
};

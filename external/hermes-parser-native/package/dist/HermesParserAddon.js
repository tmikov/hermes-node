'use strict';

const path = require('path');

const SUPPORTED = ['linux-x64', 'linux-arm64', 'darwin-x64', 'darwin-arm64'];

function loadAddon() {
  const override = process.env.HERMES_PARSER_NATIVE_ADDON;

  if (override != null && override !== '') {
    return require(path.resolve(override));
  }

  const target = `${process.platform}-${process.arch}`;
  const devBuildPath = path.join(__dirname, '..', '..', '..', '..', '..', 'cmake-build-debug', 'tools', 'hermes-parser-native', 'hermes-parser.node');
  const prebuiltPath = path.join(__dirname, '..', 'prebuilds', target, 'hermes-parser.node');

  for (const candidate of [devBuildPath, prebuiltPath]) {
    try {
      return require(candidate);
    } catch (e) {}
  }

  throw new Error(`hermes-parser-native: no prebuilt addon for ${target}. ` + `Supported platforms: ${SUPPORTED.join(', ')}. Checked the development ` + `build fallback ${devBuildPath} and ${prebuiltPath}.`);
}

module.exports = loadAddon;
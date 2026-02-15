// Minimal url shim — URL is not fully supported yet.
'use strict';

// toPathIfFileURL: if arg is a URL with file: protocol, convert to path.
// Since we don't have full URL support, just pass through non-URL values.
function toPathIfFileURL(fileURLOrPath) {
  // If it's a string that starts with file://, try to convert.
  if (typeof fileURLOrPath === 'string' && fileURLOrPath.startsWith('file://')) {
    // Basic file URL to path conversion (POSIX only).
    var path = fileURLOrPath.slice(7); // strip 'file://'
    // Handle file:///path
    if (path.startsWith('/')) {
      return decodeURIComponent(path);
    }
    return decodeURIComponent('/' + path);
  }
  // If it's an object with protocol 'file:', extract pathname.
  if (fileURLOrPath != null && typeof fileURLOrPath === 'object' &&
      fileURLOrPath.protocol === 'file:') {
    return decodeURIComponent(fileURLOrPath.pathname);
  }
  return fileURLOrPath;
}

function pathToFileURL(filepath) {
  return new URL('file://' + encodeURI(filepath));
}

function fileURLToPath(url) {
  if (typeof url === 'string') {
    return toPathIfFileURL(url);
  }
  if (url && url.protocol === 'file:') {
    return decodeURIComponent(url.pathname);
  }
  throw new TypeError('The URL must be of scheme file');
}

function isURL(value) {
  return value != null && typeof value === 'object' &&
    typeof value.href === 'string' && typeof value.protocol === 'string';
}

module.exports = {
  toPathIfFileURL,
  pathToFileURL,
  fileURLToPath,
  URL: typeof globalThis.URL !== 'undefined' ? globalThis.URL : undefined,
  isURL,
};

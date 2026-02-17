// internal/url shim backed by the native Ada URL binding.
// Provides: URL, URLSearchParams, toPathIfFileURL, pathToFileURL,
//           fileURLToPath, domainToASCII, domainToUnicode, urlToHttpOptions,
//           isURL, getURLOrigin, encodeStr, URLParse, and protocol sets.
'use strict';

var bindingUrl = internalBinding('url');

// URL component indices in the shared urlComponents Int32Array.
var kProtocolEnd = 0;
var kUsernameEnd = 1;
var kHostStart = 2;
var kHostEnd = 3;
var kPort = 4;
var kPathnameStart = 5;
var kSearchStart = 6;
var kHashStart = 7;
var kSchemeType = 8;

// Update action enum matching the native side.
var updateActions = {
  kProtocol: 0,
  kHost: 1,
  kHostname: 2,
  kPort: 3,
  kUsername: 4,
  kPassword: 5,
  kPathname: 6,
  kSearch: 7,
  kHash: 8,
  kHref: 9,
};

// This is the maximum value uint32_t can get.
// Ada uses uint32_t(-1) for declaring omitted values.
var kOmitted = 0xffffffff;
// In Int32Array, 0xffffffff reads as -1.
var kOmittedSigned = -1;

// ---------------------------------------------------------------------------
// URLContext — cached component offsets for a parsed URL
// ---------------------------------------------------------------------------
function URLContext() {
  this.href = '';
  this.protocol_end = 0;
  this.username_end = 0;
  this.host_start = 0;
  this.host_end = 0;
  this.port = 0;
  this.pathname_start = 0;
  this.search_start = 0;
  this.hash_start = 0;
  this.scheme_type = 1; // NOT_SPECIAL
}

Object.defineProperties(URLContext.prototype, {
  hasPort: {
    get: function () {
      return this.port !== kOmittedSigned;
    },
  },
  hasSearch: {
    get: function () {
      return this.search_start !== kOmittedSigned;
    },
  },
  hasHash: {
    get: function () {
      return this.hash_start !== kOmittedSigned;
    },
  },
});

function readComponents(ctx) {
  var c = bindingUrl.urlComponents;
  ctx.protocol_end = c[kProtocolEnd];
  ctx.username_end = c[kUsernameEnd];
  ctx.host_start = c[kHostStart];
  ctx.host_end = c[kHostEnd];
  ctx.port = c[kPort];
  ctx.pathname_start = c[kPathnameStart];
  ctx.search_start = c[kSearchStart];
  ctx.hash_start = c[kHashStart];
  ctx.scheme_type = c[kSchemeType];
}

// ---------------------------------------------------------------------------
// URLSearchParams
// ---------------------------------------------------------------------------

function URLSearchParams(init) {
  this._list = [];
  this._url = null; // back-reference to owning URL

  if (typeof init === 'string') {
    if (init.length > 0 && init.charCodeAt(0) === 0x3f) {
      // strip leading '?'
      init = init.slice(1);
    }
    this._parseString(init);
  } else if (Array.isArray(init)) {
    for (var i = 0; i < init.length; i++) {
      var pair = init[i];
      if (!Array.isArray(pair) || pair.length !== 2) {
        throw new TypeError(
          'Each query pair must be an iterable [name, value] pair'
        );
      }
      this._list.push([String(pair[0]), String(pair[1])]);
    }
  } else if (init != null && typeof init === 'object') {
    var keys = Object.keys(init);
    for (var j = 0; j < keys.length; j++) {
      this._list.push([String(keys[j]), String(init[keys[j]])]);
    }
  }
}

URLSearchParams.prototype._parseString = function (qs) {
  if (!qs) return;
  var pairs = qs.split('&');
  for (var i = 0; i < pairs.length; i++) {
    var pair = pairs[i];
    if (pair === '') continue;
    var eqIdx = pair.indexOf('=');
    var key, value;
    if (eqIdx === -1) {
      key = decodeURIComponent(pair.replace(/\+/g, ' '));
      value = '';
    } else {
      key = decodeURIComponent(pair.slice(0, eqIdx).replace(/\+/g, ' '));
      value = decodeURIComponent(pair.slice(eqIdx + 1).replace(/\+/g, ' '));
    }
    this._list.push([key, value]);
  }
};

URLSearchParams.prototype._notifyUrl = function () {
  if (this._url) {
    var search = this.toString();
    this._url._setSearchFromParams(search);
  }
};

URLSearchParams.prototype.append = function (name, value) {
  this._list.push([String(name), String(value)]);
  this._notifyUrl();
};

URLSearchParams.prototype.delete = function (name, value) {
  name = String(name);
  var hasValue = arguments.length > 1;
  if (hasValue) value = String(value);
  this._list = this._list.filter(function (pair) {
    if (pair[0] !== name) return true;
    if (hasValue && pair[1] !== value) return true;
    return false;
  });
  this._notifyUrl();
};

URLSearchParams.prototype.get = function (name) {
  name = String(name);
  for (var i = 0; i < this._list.length; i++) {
    if (this._list[i][0] === name) return this._list[i][1];
  }
  return null;
};

URLSearchParams.prototype.getAll = function (name) {
  name = String(name);
  var result = [];
  for (var i = 0; i < this._list.length; i++) {
    if (this._list[i][0] === name) result.push(this._list[i][1]);
  }
  return result;
};

URLSearchParams.prototype.has = function (name, value) {
  name = String(name);
  var hasValue = arguments.length > 1;
  if (hasValue) value = String(value);
  for (var i = 0; i < this._list.length; i++) {
    if (this._list[i][0] === name) {
      if (!hasValue || this._list[i][1] === value) return true;
    }
  }
  return false;
};

URLSearchParams.prototype.set = function (name, value) {
  name = String(name);
  value = String(value);
  var found = false;
  this._list = this._list.filter(function (pair) {
    if (pair[0] !== name) return true;
    if (!found) {
      found = true;
      pair[1] = value;
      return true;
    }
    return false;
  });
  if (!found) {
    this._list.push([name, value]);
  }
  this._notifyUrl();
};

URLSearchParams.prototype.sort = function () {
  this._list.sort(function (a, b) {
    if (a[0] < b[0]) return -1;
    if (a[0] > b[0]) return 1;
    return 0;
  });
  this._notifyUrl();
};

Object.defineProperty(URLSearchParams.prototype, 'size', {
  get: function () {
    return this._list.length;
  },
});

URLSearchParams.prototype.toString = function () {
  return this._list
    .map(function (pair) {
      return (
        encodeURIComponent(pair[0]).replace(/%20/g, '+') +
        '=' +
        encodeURIComponent(pair[1]).replace(/%20/g, '+')
      );
    })
    .join('&');
};

URLSearchParams.prototype.entries = function () {
  var list = this._list;
  var idx = 0;
  return {
    next: function () {
      if (idx >= list.length) return { value: undefined, done: true };
      var pair = list[idx++];
      return { value: [pair[0], pair[1]], done: false };
    },
    [Symbol.iterator]: function () {
      return this;
    },
  };
};

URLSearchParams.prototype.keys = function () {
  var list = this._list;
  var idx = 0;
  return {
    next: function () {
      if (idx >= list.length) return { value: undefined, done: true };
      return { value: list[idx++][0], done: false };
    },
    [Symbol.iterator]: function () {
      return this;
    },
  };
};

URLSearchParams.prototype.values = function () {
  var list = this._list;
  var idx = 0;
  return {
    next: function () {
      if (idx >= list.length) return { value: undefined, done: true };
      return { value: list[idx++][1], done: false };
    },
    [Symbol.iterator]: function () {
      return this;
    },
  };
};

URLSearchParams.prototype.forEach = function (callback, thisArg) {
  for (var i = 0; i < this._list.length; i++) {
    callback.call(thisArg, this._list[i][1], this._list[i][0], this);
  }
};

URLSearchParams.prototype[Symbol.iterator] =
  URLSearchParams.prototype.entries;

// ---------------------------------------------------------------------------
// URL class backed by native Ada binding
// ---------------------------------------------------------------------------

function URL(input, base) {
  if (arguments.length === 0) {
    throw new TypeError("Failed to construct 'URL': 1 argument required");
  }

  this._context = new URLContext();
  this._searchParams = null;

  input = String(input);
  var baseStr = base !== undefined ? String(base) : undefined;

  var href = bindingUrl.parse(input, baseStr, true);
  if (!href) {
    // parse with raiseException=true should have thrown; defensive fallback.
    throw new TypeError("Invalid URL: " + input);
  }

  this._context.href = href;
  readComponents(this._context);
}

// URL.parse — returns null instead of throwing on invalid input.
URL.parse = function (input, base) {
  if (arguments.length === 0) {
    throw new TypeError("Failed to call 'URL.parse': 1 argument required");
  }
  input = String(input);
  var baseStr = base !== undefined ? String(base) : undefined;
  var href = bindingUrl.parse(input, baseStr, false);
  if (!href) return null;
  var url = Object.create(URL.prototype);
  url._context = new URLContext();
  url._searchParams = null;
  url._context.href = href;
  readComponents(url._context);
  return url;
};

URL.canParse = function (input, base) {
  if (arguments.length === 0) {
    throw new TypeError("Failed to call 'URL.canParse': 1 argument required");
  }
  input = String(input);
  if (base !== undefined) {
    return bindingUrl.canParse(input, String(base));
  }
  return bindingUrl.canParse(input);
};

URL.prototype._setSearchFromParams = function (search) {
  var href;
  if (search) {
    href = bindingUrl.update(
      this._context.href,
      updateActions.kSearch,
      '?' + search
    );
  } else {
    href = bindingUrl.update(this._context.href, updateActions.kSearch, '');
  }
  if (href) {
    this._context.href = href;
    readComponents(this._context);
  }
};

URL.prototype.toString = function () {
  return this._context.href;
};

URL.prototype.toJSON = function () {
  return this._context.href;
};

Object.defineProperties(URL.prototype, {
  href: {
    get: function () {
      return this._context.href;
    },
    set: function (value) {
      value = String(value);
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kHref,
        value
      );
      if (!href) throw new TypeError('Invalid URL: ' + value);
      this._context.href = href;
      readComponents(this._context);
    },
    enumerable: true,
    configurable: true,
  },
  origin: {
    get: function () {
      var ctx = this._context;
      var protocol = ctx.href.slice(0, ctx.protocol_end);
      // NOT_SPECIAL = 1
      if (ctx.scheme_type !== 1) {
        // FILE = 6
        if (ctx.scheme_type === 6) return 'null';
        return protocol + '//' + this.host;
      }
      if (protocol === 'blob:') {
        var path = this.pathname;
        if (path.length > 0) {
          try {
            var inner = new URL(path);
            // HTTP=0, HTTPS=2
            if (
              inner._context.scheme_type === 0 ||
              inner._context.scheme_type === 2
            ) {
              return inner.protocol + '//' + inner.host;
            }
          } catch (e) {
            // ignore
          }
        }
      }
      return 'null';
    },
    enumerable: true,
    configurable: true,
  },
  protocol: {
    get: function () {
      return this._context.href.slice(0, this._context.protocol_end);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kProtocol,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  username: {
    get: function () {
      var ctx = this._context;
      // username is between protocol_end+2 (skip '//') and username_end
      if (ctx.protocol_end + 2 >= ctx.username_end) return '';
      return ctx.href.slice(ctx.protocol_end + 2, ctx.username_end);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kUsername,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  password: {
    get: function () {
      var ctx = this._context;
      // password is between username_end+1 (skip ':') and host_start
      // (host_start points at '@' when credentials exist)
      if (ctx.host_start - ctx.username_end <= 0) return '';
      return ctx.href.slice(ctx.username_end + 1, ctx.host_start);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kPassword,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  host: {
    get: function () {
      var ctx = this._context;
      var startsAt = ctx.host_start;
      // host_start might be '@' if the URL has credentials
      if (ctx.href.charCodeAt(startsAt) === 0x40) startsAt++;
      if (startsAt === ctx.host_end) return '';
      return ctx.href.slice(startsAt, ctx.pathname_start);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kHost,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  hostname: {
    get: function () {
      var ctx = this._context;
      var startsAt = ctx.host_start;
      // host_start might be '@' if the URL has credentials
      if (ctx.href.charCodeAt(startsAt) === 0x40) startsAt++;
      return ctx.href.slice(startsAt, ctx.host_end);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kHostname,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  port: {
    get: function () {
      var ctx = this._context;
      if (!ctx.hasPort) return '';
      return String(ctx.port);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kPort,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  pathname: {
    get: function () {
      var ctx = this._context;
      var endsAt = ctx.href.length;
      if (ctx.hasSearch) endsAt = ctx.search_start;
      else if (ctx.hasHash) endsAt = ctx.hash_start;
      return ctx.href.slice(ctx.pathname_start, endsAt);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kPathname,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
  search: {
    get: function () {
      var ctx = this._context;
      if (!ctx.hasSearch) return '';
      var endsAt = ctx.href.length;
      if (ctx.hasHash) endsAt = ctx.hash_start;
      if (endsAt - ctx.search_start <= 1) return '';
      return ctx.href.slice(ctx.search_start, endsAt);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kSearch,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
        // Update searchParams if they exist.
        if (this._searchParams) {
          this._searchParams._list = [];
          var qs = this.search;
          if (qs.length > 1) {
            this._searchParams._parseString(qs.slice(1));
          }
        }
      }
    },
    enumerable: true,
    configurable: true,
  },
  searchParams: {
    get: function () {
      if (!this._searchParams) {
        this._searchParams = new URLSearchParams(this.search);
        this._searchParams._url = this;
      }
      return this._searchParams;
    },
    enumerable: true,
    configurable: true,
  },
  hash: {
    get: function () {
      var ctx = this._context;
      if (!ctx.hasHash) return '';
      return ctx.href.slice(ctx.hash_start);
    },
    set: function (value) {
      var href = bindingUrl.update(
        this._context.href,
        updateActions.kHash,
        String(value)
      );
      if (href) {
        this._context.href = href;
        readComponents(this._context);
      }
    },
    enumerable: true,
    configurable: true,
  },
});

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

function isURL(self) {
  return Boolean(
    self &&
      self.href &&
      self.protocol &&
      self.auth === undefined &&
      self.path === undefined
  );
}

function urlToHttpOptions(url) {
  var hostname = url.hostname;
  var pathname = url.pathname;
  var port = url.port;
  var username = url.username;
  var password = url.password;
  var search = url.search;

  var options = {
    __proto__: null,
    protocol: url.protocol,
    hostname:
      hostname && hostname.charCodeAt(0) === 0x5b
        ? hostname.slice(1, -1)
        : hostname,
    hash: url.hash,
    search: search,
    pathname: pathname,
    path: (pathname || '') + (search || ''),
    href: url.href,
  };
  if (port !== '') {
    options.port = Number(port);
  }
  if (username || password) {
    options.auth =
      decodeURIComponent(username) + ':' + decodeURIComponent(password);
  }
  return options;
}

function domainToASCII(domain) {
  return bindingUrl.domainToASCII(String(domain));
}

function domainToUnicode(domain) {
  return bindingUrl.domainToUnicode(String(domain));
}

function getURLOrigin(url) {
  return bindingUrl.getOrigin(url);
}

function toPathIfFileURL(fileURLOrPath) {
  if (fileURLOrPath != null && typeof fileURLOrPath === 'object') {
    if (fileURLOrPath instanceof URL || fileURLOrPath.protocol === 'file:') {
      return fileURLToPath(fileURLOrPath);
    }
  }
  if (
    typeof fileURLOrPath === 'string' &&
    fileURLOrPath.startsWith('file://')
  ) {
    return fileURLToPath(fileURLOrPath);
  }
  return fileURLOrPath;
}

function fileURLToPath(path) {
  if (typeof path === 'string') {
    path = new URL(path);
  }
  if (!(path instanceof URL) && (path == null || path.protocol !== 'file:')) {
    throw new TypeError('The URL must be of scheme file');
  }
  var hostname = path.hostname;
  if (hostname && hostname !== 'localhost') {
    throw new TypeError(
      'File URL host must be "localhost" or empty on this platform'
    );
  }
  var pathname = path.pathname;
  // Check for encoded slashes.
  for (var i = 0; i < pathname.length - 2; i++) {
    if (pathname.charCodeAt(i) === 0x25) {
      // '%'
      var third = pathname.charCodeAt(i + 2) | 0x20;
      if (pathname.charCodeAt(i + 1) === 0x32 && third === 0x66) {
        // %2f or %2F
        throw new TypeError(
          'File URL path must not include encoded / characters'
        );
      }
    }
  }
  return decodeURIComponent(pathname);
}

function fileURLToPathBuffer(path) {
  // Buffer version not needed for our use case; convert via string.
  var result = fileURLToPath(path);
  if (typeof globalThis.Buffer !== 'undefined') {
    return globalThis.Buffer.from(result);
  }
  return result;
}

function pathToFileURL(filepath) {
  if (typeof filepath !== 'string') {
    throw new TypeError('The argument must be a string');
  }
  var href = bindingUrl.pathToFileURL(filepath, false);
  if (!href) {
    throw new TypeError('Invalid file path: ' + filepath);
  }
  var url = Object.create(URL.prototype);
  url._context = new URLContext();
  url._searchParams = null;
  url._context.href = href;
  readComponents(url._context);
  return url;
}

// encodeStr is used by url.js and internal/querystring. Provide a basic impl.
var hexTable = new Array(256);
for (var i = 0; i < 256; ++i) {
  hexTable[i] = '%' + (i < 16 ? '0' : '') + i.toString(16).toUpperCase();
}

function encodeStr(str, noEscapeTable, hexTable) {
  var len = str.length;
  if (len === 0) return '';

  var out = '';
  var lastPos = 0;

  for (var i = 0; i < len; i++) {
    var c = str.charCodeAt(i);
    // ASCII
    if (c < 0x80) {
      if (noEscapeTable[c] === 1) continue;
      if (lastPos < i) out += str.slice(lastPos, i);
      lastPos = i + 1;
      out += hexTable[c];
      continue;
    }

    if (lastPos < i) out += str.slice(lastPos, i);

    // Multi-byte: encode as UTF-8 percent-encoded.
    if (c < 0x800) {
      lastPos = i + 1;
      out += hexTable[0xc0 | (c >> 6)] + hexTable[0x80 | (c & 0x3f)];
    } else if (c < 0xd800 || c >= 0xe000) {
      lastPos = i + 1;
      out +=
        hexTable[0xe0 | (c >> 12)] +
        hexTable[0x80 | ((c >> 6) & 0x3f)] +
        hexTable[0x80 | (c & 0x3f)];
    } else {
      // Surrogate pair.
      ++i;
      if (i >= len) {
        // Unpaired surrogate.
        lastPos = i;
        out += '%EF%BF%BD';
        continue;
      }
      var c2 = str.charCodeAt(i);
      lastPos = i + 1;
      c = 0x10000 + (((c & 0x3ff) << 10) | (c2 & 0x3ff));
      out +=
        hexTable[0xf0 | (c >> 18)] +
        hexTable[0x80 | ((c >> 12) & 0x3f)] +
        hexTable[0x80 | ((c >> 6) & 0x3f)] +
        hexTable[0x80 | (c & 0x3f)];
    }
  }

  if (lastPos === 0) return str;
  if (lastPos < len) return out + str.slice(lastPos);
  return out;
}

function installObjectURLMethods() {
  // No-op — Blob URLs not supported.
}

// Protocol sets used by url.js.
var unsafeProtocol = new Set(['javascript', 'javascript:']);
var hostlessProtocol = new Set([
  'javascript',
  'javascript:',
]);
var slashedProtocol = new Set([
  'http',
  'http:',
  'https',
  'https:',
  'ftp',
  'ftp:',
  'gopher',
  'gopher:',
  'file',
  'file:',
]);

// Install URL and URLSearchParams as globals (WHATWG spec).
globalThis.URL = URL;
globalThis.URLSearchParams = URLSearchParams;

module.exports = {
  URL,
  URLPattern: undefined,
  URLSearchParams,
  URLParse: URL.parse,
  toPathIfFileURL,
  pathToFileURL,
  fileURLToPath,
  fileURLToPathBuffer,
  domainToASCII,
  domainToUnicode,
  urlToHttpOptions,
  isURL,
  encodeStr,
  installObjectURLMethods,
  getURLOrigin,
  unsafeProtocol,
  hostlessProtocol,
  slashedProtocol,
  urlUpdateActions: updateActions,
};

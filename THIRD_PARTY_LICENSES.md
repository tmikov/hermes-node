# Third-Party Licenses

The `hermes-node` binary statically links and/or embeds code from a
number of open-source projects. This file collects their copyright
notices and license texts as required by their respective licenses.

The file is organized by component. Where a vendored dependency
includes its own `LICENSE` file in this repository, that file is the
authoritative copy; this document either reproduces it verbatim or
points to the in-tree path.

## Hermes

JavaScript engine. Statically linked into `hermes-node`.

- Upstream: https://github.com/facebook/hermes (this repo uses a fork
  on the `n-api` branch: https://github.com/tmikov/hermes)
- License: MIT (`hermes/LICENSE`)

```
MIT License

Copyright (c) Meta Platforms, Inc. and affiliates.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following restrictions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Ada

WHATWG URL parser. Statically linked.

- Upstream: https://github.com/ada-url/ada
- License: MIT (`external/ada/ada/LICENSE-MIT`)
- Copyright: 2023 Yagiz Nizipli and Daniel Lemire

See `external/ada/ada/LICENSE-MIT` for the full text.

## Brotli

Compression library. Statically linked.

- Upstream: https://github.com/google/brotli
- License: MIT
- Copyright: 2013-present Google Inc.

The vendored source under `external/brotli/brotli/` does not include
the upstream `LICENSE` file. The applicable MIT license text:

```
Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

## c-ares

Asynchronous DNS resolver. Statically linked.

- Upstream: https://github.com/c-ares/c-ares
- License: MIT (`external/cares/cares/LICENSE.md`)
- Copyright: 1998 Massachusetts Institute of Technology; 2007-present
  Daniel Stenberg and contributors

See `external/cares/cares/LICENSE.md` for the full text.

## libuv

Asynchronous I/O library. Statically linked.

- Upstream: https://github.com/libuv/libuv
- License: MIT (`external/libuv/libuv/LICENSE`)
- Copyright: 2015-present libuv project contributors

See `external/libuv/libuv/LICENSE` for the full text. libuv also
includes additional notices in `LICENSE-extra` (third-party files
vendored within libuv) and `LICENSE-docs`.

## llhttp

HTTP parser. Statically linked.

- Upstream: https://github.com/nodejs/llhttp
- License: MIT (`external/llhttp/llhttp/LICENSE-MIT`)
- Copyright: Fedor Indutny, 2018

See `external/llhttp/llhttp/LICENSE-MIT` for the full text.

## picohash

MD5/SHA1/SHA224/SHA256 implementation in a single header. Statically
linked (compiled into the binary via a small wrapper at
`external/picohash/picohash_wrapper.c`).

- Upstream: https://github.com/kazuho/picohash
- License: Public domain (with attribution for upstream public-domain
  components)

The header `external/picohash/picohash/picohash.h` carries this
notice verbatim:

```
The code is placed under public domain by Kazuho Oku <kazuhooku@gmail.com>.

The MD5 implementation is based on a public domain implementation written by
Solar Designer <solar@openwall.com> in 2001, which is used by Dovecot.

The SHA1 implementation is based on a public domain implementation written
by Wei Dai and other contributors for libcrypt, used also in liboauth.

The SHA224/SHA256 implementation is based on a public domain implementation
by Sam Hocevar <sam@hocevar.net> for LibTomCrypt.
```

## simdutf

Unicode validation and transcoding library. Statically linked.

- Upstream: https://github.com/simdutf/simdutf
- License: Apache 2.0 / MIT dual (`external/simdutf/simdutf/LICENSE`)
- Copyright: 2021 The simdutf authors

See `external/simdutf/simdutf/LICENSE` for the full text.

## zlib

Compression library. Statically linked.

- Upstream: https://github.com/madler/zlib (this repo vendors the
  Chromium fork of zlib).
- License: zlib license (`external/zlib/zlib/LICENSE`)
- Copyright: 1995-2022 Jean-loup Gailly and Mark Adler

See `external/zlib/zlib/LICENSE` for the full text.

## zstd

Compression library. Statically linked.

- Upstream: https://github.com/facebook/zstd
- License: BSD-3-Clause (with optional dual GPLv2 in the upstream
  distribution). We redistribute only the BSD-3-Clause-licensed
  portions of the upstream source tree.
- Copyright: Meta Platforms, Inc. and affiliates

The vendored source under `external/zstd/zstd/` does not include the
upstream `LICENSE` and `COPYING` files. The applicable BSD-3-Clause
license text (which we elect under the upstream dual-license
provision):

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## ws

Node.js WebSocket library. Source vendored under `vendored/ws/` and
compiled to Hermes bytecode that is statically embedded.

- Upstream: https://github.com/websockets/ws
- License: MIT (`vendored/ws/LICENSE`)
- Copyright: Einar Otto Stangvik, Arnout Kazemier, Luigi Pinca and
  contributors

See `vendored/ws/LICENSE` for the full text.

## Node.js (libjs-node)

A subset of Node.js core JavaScript modules is vendored under
`libjs-node/` and statically embedded as Hermes bytecode.

- Upstream: https://github.com/nodejs/node
- Version: v24.13.0
- License: MIT (plus Node's bundled third-party notices)
  (`libjs-node/LICENSE`)

The Node.js `LICENSE` file in `libjs-node/LICENSE` is reproduced in
full from the upstream Node.js v24.13.0 distribution and includes
both the Node.js MIT license and the third-party license texts for
all components Node.js bundles (V8, libuv, OpenSSL, etc.). When the
license text in `libjs-node/LICENSE` covers a dependency that this
project also vendors separately (libuv, llhttp, c-ares, simdutf,
ada, zlib, brotli, zstd), the in-tree section above (which
references the dependency's own vendored copy) takes precedence.

See `libjs-node/LICENSE` for the full text.

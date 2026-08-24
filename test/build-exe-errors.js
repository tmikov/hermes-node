// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Everything --build-exe refuses after the flags have been checked: a
// container it will not link, a kit it will not link against, an output path
// it will not write, and a compiler driver that does not work. Each one has
// to say which of the two files is wrong and, where the remedy is not
// guessable, what to do about it.
//
// This file needs neither a linker nor a built kit, which is why it carries
// no `REQUIRES` line and runs on every checkout. The kit-shaped cases use a
// hand-written kit.manifest -- two lines of text is all the reader needs --
// and none of them reaches the point where the compiler driver is run,
// except the last two, which name a driver chosen to fail.
//
// The manifest's `version` has to equal this binary's own or the kit is
// refused before anything else is looked at, so the fake kits take it from
// `hermes-node --version`. That coupling is the point rather than an
// accident: both values come from HERMES_NODE_VERSION_STRING, and a build
// where they disagree is one where --build-exe would refuse every kit it
// was handed.

// A container to be refused, and one that is fine.
// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: echo "console.log('ok');" > %t.d/main.js
// RUN: %hermes-node --build-bundle=%t.d/app.hbb %t.d/main.js > /dev/null

// A container that is not there at all. The kit named here does not exist
// either, and that is deliberate: the container is checked first, so this
// also pins the order in which the two are diagnosed. A tool that reported
// the missing kit here would be answering a question the user did not ask.
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/nosuch.hbb 2>&1 | %FileCheck --check-prefix=NOCONTAINER %s
// NOCONTAINER: error: cannot open {{.*}}nosuch.hbb: No such file or directory

// A file that is not a container. Long enough to have a header's worth of
// bytes, so this is the bad-magic diagnostic and not the truncation one.
// RUN: rm -f %t.d/notabundle.js
// RUN: for i in 1 2 3 4 5 6 7 8 9 10; do echo "// filler so this is longer than a bundle header" >> %t.d/notabundle.js; done
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/notabundle.js 2>&1 | %FileCheck --check-prefix=NOTABUNDLE %s
// NOTABUNDLE: error: {{.*}}notabundle.js: hermes-node bundle: not a hermes-node bundle (bad magic)

// A real container with a byte flipped in the format version word: it has
// the magic, so this is the mismatch the reader reports rather than a
// generic "corrupt".
// RUN: cp %t.d/app.hbb %t.d/badversion.hbb
// RUN: printf 'X' | dd of=%t.d/badversion.hbb bs=1 seek=9 count=1 conv=notrunc 2>/dev/null
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/badversion.hbb 2>&1 | %FileCheck --check-prefix=BADVERSION %s
// BADVERSION: error: {{.*}}badversion.hbb: hermes-node bundle: format version mismatch

// A container built by a different hermes-node. This is the check the whole
// feature leans on -- the produced executable carries THIS binary's runtime,
// so bytecode it could not execute must be refused here rather than at the
// customer's first run -- and it is the only one of these four that a byte
// flip cannot fake by accident: the tag is stamped, four 0xff bytes over
// offset 12, the recipe bundle-dump.js and bundle-extract.js already use.
// The kit named here does not exist, which is what makes the assertion
// specific: with an intact container the same command reports the missing
// kit instead, so this line fails unless the generation check is the one
// that fired.
// RUN: cp %t.d/app.hbb %t.d/oldgen.hbb
// RUN: printf '\xff\xff\xff\xff' | dd of=%t.d/oldgen.hbb bs=1 seek=12 count=4 conv=notrunc 2>/dev/null
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/oldgen.hbb 2>&1 | %FileCheck --check-prefix=OLDGEN %s
// OLDGEN: error: {{.*}}oldgen.hbb: hermes-node bundle: built by a different hermes-node build (generation mismatch)

// A container cut short. Distinct from bad magic, and it says so.
// RUN: head -c 40 %t.d/app.hbb > %t.d/trunc.hbb
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/trunc.hbb 2>&1 | %FileCheck --check-prefix=TRUNC %s
// TRUNC: error: {{.*}}trunc.hbb: hermes-node bundle: truncated (shorter than the header)

// No kit directory. Absence is the ordinary case for a hermes-node
// installed without one, so the message names the path it looked for and
// the flag that points somewhere else.
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nokit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=NOKIT %s
// NOKIT: error: cannot open kit manifest: {{.*}}nokit/kit.manifest
// NOKIT-NEXT: note: --build-exe needs a link kit; --kit=<dir> names one.

// A directory that exists but holds no manifest -- --kit pointed one level
// off, say. The manifest is what the producer reads, so its absence is the
// same failure and gets the same message.
// RUN: mkdir -p %t.d/emptykit
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/emptykit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=EMPTYKIT %s
// EMPTYKIT: error: cannot open kit manifest: {{.*}}emptykit/kit.manifest
// EMPTYKIT-NEXT: note: --build-exe needs a link kit; --kit=<dir> names one.

// A manifest with a key this reader does not know: the kit was cut by a
// newer make-kit.py recording something that would otherwise be dropped in
// silence.
// RUN: mkdir -p %t.d/unknownkit
// RUN: printf 'version: 1\ncc: %false\nbogus: x\n' > %t.d/unknownkit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/unknownkit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=UNKNOWNKEY %s
// UNKNOWNKEY: error: {{.*}}unknownkit/kit.manifest: unknown key 'bogus'

// The two required keys, each missing in turn, and a line that is not a
// key/value pair at all (reported with its line number).
// RUN: mkdir -p %t.d/noversionkit %t.d/missingcckit %t.d/malformedkit
// RUN: printf 'cc: %false\n' > %t.d/noversionkit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/noversionkit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=NOVERSION %s
// NOVERSION: error: {{.*}}kit.manifest: missing required key 'version'
// RUN: printf 'version: 1\n' > %t.d/missingcckit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/missingcckit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=NOCC %s
// NOCC: error: {{.*}}kit.manifest: missing required key 'cc'
// RUN: printf '# a comment\nversion 1\n' > %t.d/malformedkit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/malformedkit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=MALFORMED %s
// MALFORMED: error: {{.*}}kit.manifest:2: malformed line (expected 'key: value'): version 1

// A kit cut from a different hermes-node. The archives in it were built
// against another runtime, another bytecode version and another generation
// tag, so linking them would produce a binary that fails later and further
// away. The remedy is not guessable -- hermes-node-kit is EXCLUDE_FROM_ALL,
// so an ordinary build did not re-cut it -- and the note spells it out.
// RUN: mkdir -p %t.d/oldkit
// RUN: printf 'version: 0.0.0-some-other-build\ncc: %false\n' > %t.d/oldkit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/oldkit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=OLDKIT %s
// OLDKIT: error: kit {{.*}}oldkit was cut from hermes-node 0.0.0-some-other-build, but this is hermes-node
// OLDKIT-NEXT: note: re-cut the kit with: cmake --build <build dir> --target hermes-node-kit

// From here on the fake kit's version matches this binary, so everything
// above it in the sequence has passed and the failures below are the later
// ones. That the version check lets a matching kit through is worth having
// pinned too: a check that refused everything would satisfy the case above
// just as well.
// RUN: mkdir -p %t.d/fakekit
// RUN: echo "version: $(%hermes-node --version | cut -d' ' -f2)" > %t.d/fakekit/kit.manifest
// RUN: echo "cc: %false" >> %t.d/fakekit/kit.manifest

// The output naming the container. The link would write over its own input,
// and the user would be left diagnosing a missing file they are sure they
// created.
// RUN: %not %hermes-node --build-exe=%t.d/app.hbb --kit=%t.d/fakekit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=SAMEFILE %s
// SAMEFILE: error: --build-exe={{.*}}app.hbb names the same file as the bundle {{.*}}app.hbb
// RUN: %hermes-node --bundle=%t.d/app.hbb | %FileCheck --check-prefix=STILLTHERE %s
// STILLTHERE: ok

// An output directory that does not exist. The temporaries go beside the
// output -- the directory the user chose for a large executable is the one
// known to have room for a large object file -- so this is where it is
// noticed.
// RUN: %not %hermes-node --build-exe=%t.d/nosuchdir/app.exe --kit=%t.d/fakekit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=NODIR %s
// NODIR: error: cannot write {{.*}}nosuchdir/app.exe.hnexe.

// A compiler driver that is not there. posix_spawnp's own errno, with the
// command printed so it can be rerun by hand.
// RUN: mkdir -p %t.d/nocckit
// RUN: echo "version: $(%hermes-node --version | cut -d' ' -f2)" > %t.d/nocckit/kit.manifest
// RUN: echo "cc: %t.d/no-such-compiler" >> %t.d/nocckit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/nocckit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=NOCCBIN %s
// NOCCBIN: error: cannot run {{.*}}no-such-compiler: No such file or directory
// NOCCBIN-NEXT: command: {{.*}}no-such-compiler -Qunused-arguments -c {{.*}}.s -o {{.*}}.o

// A driver that runs and fails. `false` is the smallest honest stand-in for
// a compiler that rejects what it was handed; the exit status and the
// command line are both reported, and nothing is left behind. It comes from
// the %false substitution because it is /bin/false on Linux and
// /usr/bin/false on macOS, so the CHECK lines cannot name a fixed directory
// either.
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/fakekit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=CCFAILS %s
// CCFAILS: error: {{.*}}false failed with exit status 1
// CCFAILS-NEXT: command: {{.*}}false -Qunused-arguments -c {{.*}}.s -o {{.*}}.o
// RUN: %not ls %t.d/app.exe.hnexe.*
// RUN: %not ls %t.d/app.exe

// The kit's driver flags reach the ASSEMBLE step, not only the link. On a
// universal or cross-compiling kit they are what selects the target, and a
// payload object assembled for the host cannot be linked into the other
// slice -- which is the failure the macOS release build would hit, since it
// configures CMAKE_OSX_ARCHITECTURES="x86_64;arm64" and then runs the test
// suite that cuts a kit. That link cannot be run here, so what is checked
// is the command line: `false` reports it verbatim, and the flags are in
// it, in order, before the input.
// RUN: mkdir -p %t.d/archkit
// RUN: echo "version: $(%hermes-node --version | cut -d' ' -f2)" > %t.d/archkit/kit.manifest
// RUN: printf 'cc: %false\ndriverflag: -arch\ndriverflag: hnexe-fake-arch\ndriverflag: -rdynamic\n' >> %t.d/archkit/kit.manifest
// RUN: %not %hermes-node --build-exe=%t.d/app.exe --kit=%t.d/archkit %t.d/app.hbb 2>&1 | %FileCheck --check-prefix=ARCHFLAGS %s
// ARCHFLAGS: error: {{.*}}false failed with exit status 1
// ARCHFLAGS-NEXT: command: {{.*}}false -Qunused-arguments -arch hnexe-fake-arch -rdynamic -c {{.*}}.s -o {{.*}}.o

// This file is a lit driver only; the RUN lines above are the test.

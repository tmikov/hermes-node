// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The flag-conflict matrix for --build-exe and --kit, one row per refusal.
//
// Every row appears in BOTH orders. checkToolOptions() runs after the parse
// loop precisely so that flag order cannot matter, and a check accidentally
// written inside the loop would still pass the natural order -- so the
// reversed spelling is the half that has something to prove. Each message
// must name both flags: "Error: X cannot be combined with Y" is what tells
// the user which two of the things they typed disagree.
//
// No kit and no linker are needed here, and deliberately so: every one of
// these is refused by argument checking, before a container is opened or a
// kit is read, so this file runs on every checkout. Nothing below even needs
// %t.hbb to exist. The reachable errors that come after those checks are in
// build-exe-errors.js (container and kit) and build-exe.js (output path).

// Two producers of two different artifacts, with no order to run them in.
// RUN: %not %hermes-node --build-exe=%t.exe --build-bundle=%t.hbb %s 2>&1 | %FileCheck --check-prefix=BUILDBUNDLE %s
// RUN: %not %hermes-node --build-bundle=%t.hbb --build-exe=%t.exe %s 2>&1 | %FileCheck --check-prefix=BUILDBUNDLE %s
// BUILDBUNDLE: Error: --build-exe cannot be combined with --build-bundle.

// --bundle names a container to run; --build-exe's container is its
// positional argument. Naming both asks for two jobs on what may be one file.
// RUN: %not %hermes-node --build-exe=%t.exe --bundle=%t.hbb 2>&1 | %FileCheck --check-prefix=BUNDLE %s
// RUN: %not %hermes-node --bundle=%t.hbb --build-exe=%t.exe 2>&1 | %FileCheck --check-prefix=BUNDLE %s
// BUNDLE: Error: --build-exe cannot be combined with --bundle.

// The four read-only verbs, each against --build-exe.
// RUN: %not %hermes-node --build-exe=%t.exe --dump %t.hbb 2>&1 | %FileCheck --check-prefix=DUMP %s
// RUN: %not %hermes-node --dump --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=DUMP %s
// DUMP: Error: --build-exe cannot be combined with --dump.

// RUN: %not %hermes-node --build-exe=%t.exe --extract-module=x %t.hbb 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// RUN: %not %hermes-node --extract-module=x --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// EXTRACT: Error: --build-exe cannot be combined with --extract-module.

// RUN: %not %hermes-node --build-exe=%t.exe --dump-bytecode=%t.bc %t.hbb 2>&1 | %FileCheck --check-prefix=DUMPBC %s
// RUN: %not %hermes-node --dump-bytecode=%t.bc --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=DUMPBC %s
// DUMPBC: Error: --build-exe cannot be combined with --dump-bytecode.

// RUN: %not %hermes-node --build-exe=%t.exe --verify-natives %t.hbb 2>&1 | %FileCheck --check-prefix=VERIFY %s
// RUN: %not %hermes-node --verify-natives --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=VERIFY %s
// VERIFY: Error: --build-exe cannot be combined with --verify-natives.

// -e/--eval supplies a program instead of reading one; --build-exe links one
// that was already built. Neither leaves anything for the other to do. Both
// spellings of the flag, since they are parsed separately.
// RUN: %not %hermes-node --build-exe=%t.exe -e "1" %t.hbb 2>&1 | %FileCheck --check-prefix=EVAL %s
// RUN: %not %hermes-node -e "1" --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=EVAL %s
// RUN: %not %hermes-node --eval "1" --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=EVAL %s
// EVAL: Error: --build-exe cannot be combined with -e or --eval.

// Nothing runs while a verb runs, so there is nothing for an inspector
// session to attach to. The message says so in its own words, which is a
// different reason from the one --bundle gives, and both flags are named.
// RUN: %not %hermes-node --build-exe=%t.exe --inspect %t.hbb 2>&1 | %FileCheck --check-prefix=INSPECT %s
// RUN: %not %hermes-node --inspect --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=INSPECT %s
// RUN: %not %hermes-node --build-exe=%t.exe --inspect-brk %t.hbb 2>&1 | %FileCheck --check-prefix=INSPECT %s
// RUN: %not %hermes-node --inspect-brk --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=INSPECT %s
// INSPECT: Error: --build-exe cannot be combined with --inspect or --inspect-brk.
// INSPECT-NEXT: --build-exe describes a file, it does not run one, so there would be nothing to inspect.

// The container is the positional argument, the same slot a script path
// occupies everywhere else. There is nothing to link without one.
// RUN: %not %hermes-node --build-exe=%t.exe 2>&1 | %FileCheck --check-prefix=NOCONTAINER %s
// NOCONTAINER: Error: --build-exe requires a bundle file argument, e.g. hermes-node --build-exe=<output> <bundle.hbb>.

// --kit says where the prebuilt kit lives, and only --build-exe consumes
// one. Both orders again, and once with no other flag at all.
// RUN: %not %hermes-node --kit=%t.kit %s 2>&1 | %FileCheck --check-prefix=KITALONE %s
// RUN: %not %hermes-node --kit=%t.kit --bundle=%t.hbb 2>&1 | %FileCheck --check-prefix=KITALONE %s
// RUN: %not %hermes-node --bundle=%t.hbb --kit=%t.kit 2>&1 | %FileCheck --check-prefix=KITALONE %s
// KITALONE: Error: --kit requires --build-exe.

// An empty value is a flag naming a file that cannot exist. Both flags say
// so with the flag's own name in the message, rather than letting a
// diagnostic with no filename in it out of the door.
// RUN: %not %hermes-node --build-exe= %t.hbb 2>&1 | %FileCheck --check-prefix=EMPTYEXE %s
// EMPTYEXE: Error: --build-exe requires a file path.
// RUN: %not %hermes-node --build-exe=%t.exe --kit= %t.hbb 2>&1 | %FileCheck --check-prefix=EMPTYKIT %s
// RUN: %not %hermes-node --kit= --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=EMPTYKIT %s
// EMPTYKIT: Error: --kit requires a directory path.

// --cc names the driver the assemble and link run, so like --kit it has no
// consumer outside --build-exe: every other verb reads a file and runs no
// toolchain. Both orders, and once alone.
// RUN: %not %hermes-node --cc=c++ %s 2>&1 | %FileCheck --check-prefix=CCALONE %s
// RUN: %not %hermes-node --cc=c++ --bundle=%t.hbb 2>&1 | %FileCheck --check-prefix=CCALONE %s
// RUN: %not %hermes-node --bundle=%t.hbb --cc=c++ 2>&1 | %FileCheck --check-prefix=CCALONE %s
// CCALONE: Error: --cc requires --build-exe.

// An empty --cc is a driver with no name. Refused by the flag's own name,
// rather than as a spawn failure with nothing in it to act on.
// RUN: %not %hermes-node --build-exe=%t.exe --cc= %t.hbb 2>&1 | %FileCheck --check-prefix=EMPTYCC %s
// RUN: %not %hermes-node --cc= --build-exe=%t.exe %t.hbb 2>&1 | %FileCheck --check-prefix=EMPTYCC %s
// EMPTYCC: Error: --cc requires a compiler name or path.

// --verbose has five consumers now, and --build-exe is the fifth: the
// message has to list it, or the flag it accepts is one it does not admit to.
// RUN: %not %hermes-node --verbose %s 2>&1 | %FileCheck --check-prefix=VERBOSE %s
// VERBOSE: Error: --verbose requires --build-bundle, --dump, --verify-natives, --dump-bytecode or --build-exe.

// A flag typed AFTER the container. The parse loop stops at the first
// positional -- everything past it belongs to the program being run -- and
// --build-exe is the only verb whose own input sits in that slot, so it is
// the only one where the convention is observable. It used to be
// observable only by the flag having no effect: --verbose narrated nothing
// and --kit silently used the default. Both are refused now, by name.
// RUN: %not %hermes-node --build-exe=%t.exe %t.hbb --verbose 2>&1 | %FileCheck --check-prefix=LATEVERBOSE %s
// LATEVERBOSE: Error: '--verbose' appears after the bundle file '{{.*}}.hbb'; options must come before it.
// RUN: %not %hermes-node --build-exe=%t.exe %t.hbb --kit=%t.kit 2>&1 | %FileCheck --check-prefix=LATEKIT %s
// LATEKIT: Error: '--kit={{.*}}' appears after the bundle file '{{.*}}.hbb'; options must come before it.

// Including a flag that does not exist: it is refused for being late
// rather than for being unknown, because the parse loop never saw it. The
// message still names it, which is what the user needs either way.
// RUN: %not %hermes-node --build-exe=%t.exe %t.hbb --no-such-flag 2>&1 | %FileCheck --check-prefix=LATEUNKNOWN %s
// LATEUNKNOWN: Error: '--no-such-flag' appears after the bundle file '{{.*}}.hbb'; options must come before it.

// The same flags before the container are accepted, which is what makes the
// three rows above mean "too late" rather than "rejected everywhere". Both
// are consumed here and the run gets all the way past argument checking, to
// the container that -- as everywhere in this file -- was never created.
// RUN: %not %hermes-node --build-exe=%t.exe --verbose --kit=%t.kit %t.hbb 2>&1 | %FileCheck --check-prefix=EARLYOK %s
// EARLYOK: error: cannot open {{.*}}.hbb: No such file or directory

// This file is a lit driver only; the RUN lines above are the test.

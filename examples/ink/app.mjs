// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A small interactive TUI exercising the parts of Ink that only recently
// started working here: Yoga layout (WebAssembly), a timer driving
// re-renders, useInput's raw-mode keyboard handling, and clean exit via
// useApp(). See ../../docs/notes/2026-08-24-ink-findings.md for how each
// of those landed.
//
// No JSX: React.createElement directly, so the build needs no JSX loader.
import React, {useState, useEffect} from 'react';
import {render, Text, Box, useInput, useApp} from 'ink';
import {initYoga} from './yoga-shim.mjs';

// Grapheme-segmentation exhibit. Each box is sized to its own content with
// no explicit width, so the right border lands wherever Ink's width
// calculation (via string-width, which walks graphemes with
// Intl.Segmenter) says the text ends. Get segmentation wrong -- count
// UTF-16 code units, or code points instead of grapheme clusters -- and a
// ZWJ family emoji or a skin-tone modifier is miscounted as several
// "characters" instead of one, so the border for that row lands in the
// wrong place relative to what a terminal actually draws. The CJK row is
// the same idea from the other direction: each character is one grapheme
// but two terminal cells wide, which string-width also has to get right.
// A run under node vs. hermes-node produces byte-identical padding for all
// three -- see run.sh -- which is the point of the exhibit.
const rows = [
  {label: 'zwj', text: '\u{1F468}‍\u{1F469}‍\u{1F467}‍\u{1F466} family'},
  {label: 'tone', text: '\u{1F44B}\u{1F3FD} wave'},
  {label: 'cjk', text: '你好世界 hello'},
];

function App() {
  const {exit} = useApp();
  const [tick, setTick] = useState(0);
  const [lastKey, setLastKey] = useState('none');

  useEffect(() => {
    const id = setInterval(() => setTick((t) => t + 1), 500);
    return () => clearInterval(id);
  }, []);

  // pty-run.py's --send delivers its whole argument as a single keypress,
  // not one event per character, so a multi-character paste like "abcq"
  // never equals the string 'q' and cannot trigger this handler -- only an
  // actual, solitary 'q' keypress does. run.sh relies on that distinction
  // to tell "kept running" apart from "quit".
  useInput((input, key) => {
    setLastKey(key.escape ? 'esc' : JSON.stringify(input));
    if (input === 'q' || key.escape) {
      exit();
    }
  });

  return React.createElement(
    Box,
    {flexDirection: 'column', borderStyle: 'round', paddingX: 1},
    React.createElement(Text, {bold: true}, 'hermes-node + Ink 6.4.0'),
    React.createElement(
      Text,
      null,
      `tick ${tick} -- last key ${lastKey} -- q to quit`
    ),
    ...rows.map((r) =>
      React.createElement(
        Box,
        {key: r.label, borderStyle: 'single'},
        React.createElement(Text, null, r.text)
      )
    )
  );
}

initYoga().then(() => {
  const instance = render(React.createElement(App));
  // A fixed marker for run.sh: its presence proves useApp().exit() led to
  // a clean shutdown rather than the harness's own timeout killing the
  // process.
  instance.waitUntilExit().then(() => {
    console.log('EXITED CLEANLY');
  });
});

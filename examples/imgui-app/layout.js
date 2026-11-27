// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The default layout: ImGui's own settings text for the six panels docked
// as README.md's sketch arranges them, loaded with
// imgui.loadIniSettingsFromMemory() before the first frame and again by
// View > Reset Layout. It is what imgui.saveIniSettingsToMemory() returned
// for that arrangement, less the fallback window's section
// ([Window][Debug##Default]) and the LastUsed= lines (the date of the
// run); README.md, "The default layout", gives the steps to regenerate it.
//
// Only ever load text that saveIniSettingsToMemory() produced: ImGui does
// not validate this format, and a malformed string can hang it or abort
// the process.
//
// The dock space's ID, 0x08BD597D, is the one imgui.dockSpaceOverViewport()
// gives a zero argument for the main viewport; node IDs 1..10 were chosen
// by hand. Pos/Size are for a 1280x800 window under a 19-unit menu bar;
// ImGui rescales the nodes to the window it finds.

'use strict';

const DEFAULT = [
  '[Window][Files]',
  'Pos=0,19',
  'Size=380,390',
  'Collapsed=0',
  'DockId=0x00000003,0',
  '',
  '[Window][Viewer]',
  'Pos=0,411',
  'Size=380,389',
  'Collapsed=0',
  'DockId=0x00000004,0',
  '',
  '[Window][Widgets]',
  'Pos=382,19',
  'Size=518,520',
  'Collapsed=0',
  'DockId=0x00000007,0',
  '',
  '[Window][Log]',
  'Pos=382,541',
  'Size=518,259',
  'Collapsed=0',
  'DockId=0x00000008,0',
  '',
  '[Window][Monitor]',
  'Pos=902,19',
  'Size=378,330',
  'Collapsed=0',
  'DockId=0x00000009,0',
  '',
  '[Window][Canvas]',
  'Pos=902,351',
  'Size=378,449',
  'Collapsed=0',
  'DockId=0x0000000A,0',
  '',
  '[Window][WindowOverViewport_11111111]',
  'Pos=0,19',
  'Size=1280,781',
  'Collapsed=0',
  '',
  '[Docking][Data]',
  'DockSpace       ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1280,781 Split=X',
  '  DockNode      ID=0x00000001 Parent=0x08BD597D SizeRef=380,781 Split=Y',
  '    DockNode    ID=0x00000003 Parent=0x00000001 SizeRef=380,390 Selected=0x0469CA7A',
  '    DockNode    ID=0x00000004 Parent=0x00000001 SizeRef=380,389 Selected=0xF8864585',
  '  DockNode      ID=0x00000002 Parent=0x08BD597D SizeRef=898,781 Split=X',
  '    DockNode    ID=0x00000005 Parent=0x00000002 SizeRef=518,781 Split=Y',
  '      DockNode  ID=0x00000007 Parent=0x00000005 SizeRef=518,520 CentralNode=1 Selected=0x80CC8223',
  '      DockNode  ID=0x00000008 Parent=0x00000005 SizeRef=518,259 Selected=0x139FDA3F',
  '    DockNode    ID=0x00000006 Parent=0x00000002 SizeRef=378,781 Split=Y',
  '      DockNode  ID=0x00000009 Parent=0x00000006 SizeRef=378,330 Selected=0x160339FF',
  '      DockNode  ID=0x0000000A Parent=0x00000006 SizeRef=378,449 Selected=0x5EE3988C',
  '',
].join('\n');

module.exports = {DEFAULT};

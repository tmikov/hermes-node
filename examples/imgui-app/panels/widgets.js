// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Widgets: a gallery in tabs. Basic (buttons, checkbox, radio buttons,
// sliders, drags, inputs, a combo, colour editors, a progress bar, a list
// box); a table with a frozen header row filled from os.cpus() and
// process.versions, drawn through the list clipper; trees and collapsing
// headers; popups (a context menu, a modal) and tooltips.
//
// Every value lives in a one-element (or N-element) array, the binding's
// reference arrays: the widget reads it and writes the edit back.

'use strict';

const os = require('os');

const TABS = ['Basic', 'Table', 'Trees', 'Popups'];
const COMBO_ITEMS = ['dark', 'light', 'classic', 'solarized', 'mono'];

const v = {
  clicks: 0,
  check: [true],
  radio: [0],
  slider: [0.42],
  sliderInt: [7],
  drag: [1.5],
  dragInt: [3],
  text: ['hello'],
  hint: [''],
  inputFloat: [3.14],
  inputInt: [42],
  combo: 0,
  color3: [0.2, 0.6, 1],
  color4: [1, 0.5, 0.2, 1],
  list: 0,
  headerVisible: [true],
  contextChoice: 'none',
  modalCount: 0,
};

let systemRows = null;

/** os.cpus() and process.versions as table rows, read once. */
function rows() {
  if (systemRows !== null)
    return systemRows;
  systemRows = [];
  os.cpus().forEach((cpu, i) => {
    systemRows.push(['cpu ' + i, cpu.model, cpu.speed + ' MHz']);
  });
  for (const [name, version] of Object.entries(process.versions))
    systemRows.push(['version', name, String(version)]);
  return systemRows;
}

const panel = {
  name: 'Widgets',
  open: [true],

  /**
   * When set in test mode (`driven`), the Table tab's list clipper
   * callback throws, and clears it: a throw from inside a clipper
   * callback, a table and a child window, for test/ui-imgui-app.js. No
   * read-back or injected input can make application code throw, so this
   * one control stays; outside test mode it is ignored.
   */
  throwNextFrame: false,

  draw(ctx) {
    const {imgui} = ctx;
    if (!imgui.beginTabBar('gallery'))
      return;
    for (const tab of TABS) {
      const visible = imgui.beginTabItem(tab);
      // The tab itself is the last item, contents or not (app.rects).
      ctx.state.recordRect('tab:' + tab);
      if (visible) {
        DRAW[tab](ctx);
        imgui.endTabItem();
      }
    }
    imgui.endTabBar();
  },
};

function drawBasic(ctx) {
  const {imgui} = ctx;
  if (imgui.button('Button'))
    ++v.clicks;
  imgui.sameLine();
  imgui.text('clicked ' + v.clicks + (v.clicks === 1 ? ' time' : ' times'));
  imgui.sameLine();
  if (imgui.smallButton('reset'))
    v.clicks = 0;
  imgui.checkbox('Check', v.check);
  imgui.radioButton('one', v.radio, 0);
  imgui.sameLine();
  imgui.radioButton('two', v.radio, 1);
  imgui.sameLine();
  imgui.radioButton('three', v.radio, 2);
  imgui.sliderFloat('Slider', v.slider, 0, 1);
  imgui.sliderInt('Int slider', v.sliderInt, 0, 100);
  imgui.dragFloat('Drag', v.drag, 0.01, 0, 10);
  imgui.dragInt('Drag int', v.dragInt, 0.2, 0, 100);
  imgui.inputText('Text', v.text, 64);
  imgui.inputTextWithHint('Hint', 'type here', v.hint, 64);
  imgui.inputFloat('Float', v.inputFloat, 0.1, 1, '%.3f');
  imgui.inputInt('Int', v.inputInt);
  if (imgui.beginCombo('Combo', COMBO_ITEMS[v.combo])) {
    COMBO_ITEMS.forEach((item, i) => {
      if (imgui.selectable(item, i === v.combo))
        v.combo = i;
      if (i === v.combo)
        imgui.setItemDefaultFocus();
    });
    imgui.endCombo();
  }
  imgui.colorEdit3('Color', v.color3);
  imgui.colorEdit4('Color 4', v.color4);
  imgui.colorButton('swatch', v.color4);
  imgui.sameLine();
  imgui.text('a colour button');
  const fraction = 0.5 + 0.5 * Math.sin(ctx.time);
  imgui.progressBar(fraction, [-imgui.FLT_MIN, 0],
      Math.round(fraction * 100) + '%');
  const listHeight = 5 * imgui.getTextLineHeightWithSpacing();
  if (imgui.beginListBox('List', [0, listHeight])) {
    COMBO_ITEMS.forEach((item, i) => {
      if (imgui.selectable(item, i === v.list))
        v.list = i;
    });
    imgui.endListBox();
  }
}

function drawTable(ctx) {
  const {imgui} = ctx;
  const all = rows();
  const rowHeight = imgui.getTextLineHeightWithSpacing();
  if (imgui.beginChild('system', [0, 0], imgui.ChildFlags_Borders)) {
    if (imgui.beginTable('system', 3,
            imgui.TableFlags_ScrollY | imgui.TableFlags_RowBg |
                imgui.TableFlags_BordersOuter | imgui.TableFlags_BordersV |
                imgui.TableFlags_Resizable)) {
      imgui.tableSetupScrollFreeze(0, 1);
      imgui.tableSetupColumn('Kind', imgui.TableColumnFlags_WidthFixed);
      imgui.tableSetupColumn('Name');
      imgui.tableSetupColumn('Value');
      imgui.tableHeadersRow();
      imgui.listClipper(all.length, rowHeight, (start, end) => {
        for (let i = start; i < end; ++i) {
          imgui.tableNextRow(0, rowHeight);
          for (const cell of all[i]) {
            imgui.tableNextColumn();
            imgui.text(cell);
          }
          if (ctx.testMode && panel.throwNextFrame) {
            panel.throwNextFrame = false;
            throw new Error('thrown from the list clipper\'s callback');
          }
        }
      });
      imgui.endTable();
    }
  }
  imgui.endChild();
}

function drawTrees(ctx) {
  const {imgui} = ctx;
  if (imgui.treeNode('Process')) {
    imgui.bulletText('pid ' + process.pid);
    imgui.bulletText('platform ' + process.platform + ' ' + process.arch);
    if (imgui.treeNode('argv')) {
      process.argv.forEach((arg, i) => imgui.bulletText(i + ': ' + arg));
      imgui.treePop();
    }
    imgui.treePop();
  }
  imgui.setNextItemOpen(true, imgui.Cond_Once);
  if (imgui.treeNodeEx('Operating system', imgui.TreeNodeFlags_Framed)) {
    imgui.labelText('type', os.type());
    imgui.labelText('release', os.release());
    imgui.labelText('cpus', String(os.cpus().length));
    imgui.treePop();
  }
  if (imgui.collapsingHeader('A collapsing header')) {
    imgui.textWrapped(
        'Collapsing headers are tree nodes that do not indent their ' +
        'contents.');
  }
  if (v.headerVisible[0] &&
      imgui.collapsingHeader('A closable header', v.headerVisible)) {
    imgui.text('Its close button clears the array passed in.');
  }
  if (!v.headerVisible[0] && imgui.button('Bring the header back'))
    v.headerVisible[0] = true;
}

function drawPopups(ctx) {
  const {imgui} = ctx;
  imgui.button('Right-click me');
  if (imgui.beginPopupContextItem('context')) {
    for (const choice of ['copy', 'paste', 'delete']) {
      if (imgui.menuItem(choice))
        v.contextChoice = choice;
    }
    imgui.endPopup();
  }
  imgui.sameLine();
  imgui.text('last choice: ' + v.contextChoice);

  if (imgui.button('Open a modal'))
    imgui.openPopup('A modal');
  if (imgui.beginPopupModal('A modal', null,
          imgui.WindowFlags_AlwaysAutoResize)) {
    imgui.text('Opened ' + (v.modalCount + 1) + ' time(s).');
    if (imgui.button('Close')) {
      ++v.modalCount;
      imgui.closeCurrentPopup();
    }
    imgui.endPopup();
  }

  imgui.button('Hover me');
  imgui.setItemTooltip('A tooltip for the last item.');
  imgui.sameLine();
  imgui.textDisabled('(?)');
  if (imgui.isItemHovered() && imgui.beginTooltip()) {
    imgui.text('A tooltip with contents of its own:');
    imgui.progressBar(0.7, [160, 0]);
    imgui.endTooltip();
  }
  imgui.text('Plain:');
  imgui.sameLine();
  imgui.textDisabled('hover');
  if (imgui.isItemHovered())
    imgui.setTooltip('setTooltip: a plain string.');
}

const DRAW = {
  Basic: drawBasic,
  Table: drawTable,
  Trees: drawTrees,
  Popups: drawPopups,
};

module.exports = panel;

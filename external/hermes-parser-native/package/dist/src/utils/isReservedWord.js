'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = isReservedWord;

function isReservedWord(name) {
  switch (name) {
    case 'await':
    case 'break':
    case 'case':
    case 'catch':
    case 'class':
    case 'const':
    case 'continue':
    case 'debugger':
    case 'default':
    case 'delete':
    case 'do':
    case 'else':
    case 'enum':
    case 'export':
    case 'extends':
    case 'false':
    case 'finally':
    case 'for':
    case 'function':
    case 'if':
    case 'import':
    case 'in':
    case 'instanceof':
    case 'new':
    case 'null':
    case 'return':
    case 'super':
    case 'switch':
    case 'this':
    case 'throw':
    case 'true':
    case 'try':
    case 'typeof':
    case 'var':
    case 'void':
    case 'while':
    case 'with':
    case 'yield':
      return true;

    default:
      return false;
  }
}
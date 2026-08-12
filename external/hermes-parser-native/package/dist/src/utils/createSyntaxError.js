'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.createSyntaxError = createSyntaxError;

function createSyntaxError(node, err) {
  const syntaxError = new SyntaxError(err);
  syntaxError.loc = {
    line: node.loc.start.line,
    column: node.loc.start.column
  };
  return syntaxError;
}
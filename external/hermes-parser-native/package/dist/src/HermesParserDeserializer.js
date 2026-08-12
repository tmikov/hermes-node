'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = void 0;

var _HermesParserDecodeUTF8String = _interopRequireDefault(require("./HermesParserDecodeUTF8String"));

var _HermesParserNodeDeserializers = _interopRequireDefault(require("./HermesParserNodeDeserializers"));

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

class HermesParserDeserializer {
  constructor(buffer, header, options) {
    this.programBufferIdx = void 0;
    this.positionBufferIdx = void 0;
    this.positionBufferSize = void 0;
    this.locMap = void 0;
    this.programBuffer = void 0;
    this.programFloats = void 0;
    this.positionBuffer = void 0;
    this.stringOffsets = void 0;
    this.stringData = void 0;
    this.stringCache = void 0;
    this.options = void 0;
    this.commentTypes = ['CommentLine', 'CommentBlock', 'InterpreterDirective'];
    this.tokenTypes = ['Boolean', 'Identifier', 'Keyword', 'Null', 'Numeric', 'BigInt', 'Punctuator', 'String', 'RegularExpression', 'Template', 'JSXText'];
    const programOffset = header[3];
    const programLength = header[4];
    const positionOffset = header[5];
    const positionCount = header[6];
    const strOffsetsOffset = header[7];
    const stringCount = header[8];
    const strDataOffset = header[9];
    const strDataLength = header[10];
    this.programBuffer = new Uint32Array(buffer, programOffset, programLength);
    this.programFloats = new Float64Array(buffer, programOffset, programLength >> 1);
    this.positionBuffer = new Uint32Array(buffer, positionOffset, positionCount * 5);
    this.stringOffsets = new Uint32Array(buffer, strOffsetsOffset, stringCount + 1);
    this.stringData = new Uint8Array(buffer, strDataOffset, strDataLength);
    this.stringCache = new Array(stringCount);
    this.programBufferIdx = 0;
    this.positionBufferIdx = 0;
    this.positionBufferSize = positionCount;
    this.locMap = {};
    this.options = options;
  }

  next() {
    const num = this.programBuffer[this.programBufferIdx++];
    return num;
  }

  getString(id) {
    const cached = this.stringCache[id];

    if (cached !== undefined) {
      return cached;
    }

    const start = this.stringOffsets[id];
    const end = this.stringOffsets[id + 1];
    const str = (0, _HermesParserDecodeUTF8String.default)(start, end - start, this.stringData);
    this.stringCache[id] = str;
    return str;
  }

  deserialize() {
    const program = {
      type: 'Program',
      loc: this.addEmptyLoc(),
      body: this.deserializeNodeList(),
      comments: this.deserializeComments()
    };

    if (this.options.tokens === true) {
      program.tokens = this.deserializeTokens();
    }

    this.fillLocs();
    return program;
  }

  deserializeBoolean() {
    return Boolean(this.next());
  }

  deserializeNumber() {
    let floatIdx;

    if (this.programBufferIdx % 2 === 0) {
      floatIdx = this.programBufferIdx / 2;
      this.programBufferIdx += 2;
    } else {
      floatIdx = (this.programBufferIdx + 1) / 2;
      this.programBufferIdx += 3;
    }

    return this.programFloats[floatIdx];
  }

  deserializeString() {
    const id = this.next();

    if (id === 0) {
      return null;
    }

    return this.getString(id - 1);
  }

  deserializeNode() {
    const nodeType = this.next();

    if (nodeType === 0) {
      return null;
    }

    const nodeDeserializer = _HermesParserNodeDeserializers.default[nodeType - 1].bind(this);

    return nodeDeserializer();
  }

  deserializeNodeList() {
    const size = this.next();
    const nodeList = [];

    for (let i = 0; i < size; i++) {
      nodeList.push(this.deserializeNode());
    }

    return nodeList;
  }

  deserializeComments() {
    const size = this.next();
    const comments = [];

    for (let i = 0; i < size; i++) {
      const commentType = this.commentTypes[this.next()];
      const loc = this.addEmptyLoc();
      const value = this.deserializeString();
      comments.push({
        type: commentType,
        loc,
        value
      });
    }

    return comments;
  }

  deserializeTokens() {
    const size = this.next();
    const tokens = [];

    for (let i = 0; i < size; i++) {
      const tokenType = this.tokenTypes[this.next()];
      const loc = this.addEmptyLoc();
      const value = this.deserializeString();
      tokens.push({
        type: tokenType,
        loc,
        value
      });
    }

    return tokens;
  }

  addEmptyLoc() {
    const loc = {};
    this.locMap[this.next()] = loc;
    return loc;
  }

  fillLocs() {
    for (let i = 0; i < this.positionBufferSize; i++) {
      const locId = this.positionBuffer[this.positionBufferIdx++];
      const kind = this.positionBuffer[this.positionBufferIdx++];
      const line = this.positionBuffer[this.positionBufferIdx++];
      const column = this.positionBuffer[this.positionBufferIdx++];
      const offset = this.positionBuffer[this.positionBufferIdx++];
      const loc = this.locMap[locId];

      if (kind === 0) {
        loc.start = {
          line,
          column
        };
        loc.rangeStart = offset;
      } else {
        loc.end = {
          line,
          column
        };
        loc.rangeEnd = offset;
      }
    }
  }

}

exports.default = HermesParserDeserializer;
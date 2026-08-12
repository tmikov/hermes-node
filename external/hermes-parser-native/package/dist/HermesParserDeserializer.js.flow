/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 */

'use strict';

import type {
  HermesSourceLocation,
  HermesNode,
  HermesToken,
  HermesComment,
} from './HermesAST';
import type {ParserOptions} from './ParserOptions';

import HermesParserDecodeUTF8String from './HermesParserDecodeUTF8String';
import NODE_DESERIALIZERS from './HermesParserNodeDeserializers';

export default class HermesParserDeserializer {
  programBufferIdx: number;
  positionBufferIdx: number;
  readonly positionBufferSize: number;
  readonly locMap: {[number]: HermesSourceLocation};
  readonly programBuffer: Uint32Array;
  readonly programFloats: Float64Array;
  readonly positionBuffer: Uint32Array;
  readonly stringOffsets: Uint32Array;
  readonly stringData: Uint8Array;
  readonly stringCache: Array<string | void>;
  readonly options: ParserOptions;

  // Matches StoredComment::Kind enum in JSLexer.h
  readonly commentTypes: ReadonlyArray<HermesComment['type']> = [
    'CommentLine',
    'CommentBlock',
    'InterpreterDirective',
  ];

  // Matches TokenType enum in HermesParserJSSerializer.h
  readonly tokenTypes: ReadonlyArray<HermesToken['type']> = [
    'Boolean',
    'Identifier',
    'Keyword',
    'Null',
    'Numeric',
    'BigInt',
    'Punctuator',
    'String',
    'RegularExpression',
    'Template',
    'JSXText',
  ];

  constructor(
    buffer: ArrayBuffer,
    header: Uint32Array,
    options: ParserOptions,
  ) {
    const programOffset = header[3];
    const programLength = header[4];
    const positionOffset = header[5];
    const positionCount = header[6];
    const strOffsetsOffset = header[7];
    const stringCount = header[8];
    const strDataOffset = header[9];
    const strDataLength = header[10];

    this.programBuffer = new Uint32Array(
      buffer,
      programOffset,
      programLength,
    );
    this.programFloats = new Float64Array(
      buffer,
      programOffset,
      programLength >> 1,
    );
    this.positionBuffer = new Uint32Array(
      buffer,
      positionOffset,
      positionCount * 5,
    );
    this.stringOffsets = new Uint32Array(
      buffer,
      strOffsetsOffset,
      stringCount + 1,
    );
    this.stringData = new Uint8Array(buffer, strDataOffset, strDataLength);
    this.stringCache = new Array(stringCount);

    // Indices are region-relative, so both start at zero rather than at a
    // pointer divided by four.
    this.programBufferIdx = 0;
    this.positionBufferIdx = 0;
    this.positionBufferSize = positionCount;

    this.locMap = {};
    this.options = options;
  }

  /**
   * Consume and return the next 4 bytes in the program buffer.
   */
  next(): number {
    const num = this.programBuffer[this.programBufferIdx++];
    return num;
  }

  /**
   * Decode string `id` from the table, caching the result so each unique
   * string is decoded exactly once and every reference shares one JS string.
   */
  getString(id: number): string {
    const cached = this.stringCache[id];
    if (cached !== undefined) {
      return cached;
    }
    const start = this.stringOffsets[id];
    const end = this.stringOffsets[id + 1];
    const str = HermesParserDecodeUTF8String(
      start,
      end - start,
      this.stringData,
    );
    this.stringCache[id] = str;
    return str;
  }

  deserialize(): HermesNode {
    const program: HermesNode = {
      type: 'Program',
      loc: this.addEmptyLoc(),
      body: this.deserializeNodeList(),
      comments: this.deserializeComments(),
    };

    if (this.options.tokens === true) {
      program.tokens = this.deserializeTokens();
    }

    this.fillLocs();

    return program;
  }

  /**
   * Booleans are serialized as a single 4-byte integer.
   */
  deserializeBoolean(): boolean {
    return Boolean(this.next());
  }

  /**
   * Numbers are serialized directly into program buffer, taking up 8 bytes
   * preceded by 4 bytes of alignment padding if necessary.
   */
  deserializeNumber(): number {
    let floatIdx;

    // Numbers are aligned on 8-byte boundaries, so skip padding if we are at
    // an odd index into the 4-byte aligned program buffer.
    if (this.programBufferIdx % 2 === 0) {
      floatIdx = this.programBufferIdx / 2;
      this.programBufferIdx += 2;
    } else {
      floatIdx = (this.programBufferIdx + 1) / 2;
      this.programBufferIdx += 3;
    }

    return this.programFloats[floatIdx];
  }

  /**
   * Strings are serialized as a single string-table id biased by one, so that
   * zero represents a null string.
   */
  deserializeString(): ?string {
    const id = this.next();
    if (id === 0) {
      return null;
    }
    return this.getString(id - 1);
  }

  /**
   * Nodes are serialized as a 4-byte integer denoting their node kind,
   * followed by a 4-byte loc ID, followed by serialized node properties.
   *
   * If the node kind is 0 the node is null, otherwise the node kind - 1 is an
   * index into the array of node deserialization functions.
   */
  deserializeNode(): ?HermesNode {
    const nodeType = this.next();
    if (nodeType === 0) {
      return null;
    }

    const nodeDeserializer = NODE_DESERIALIZERS[nodeType - 1].bind(this);
    return nodeDeserializer();
  }

  /**
   * Node lists are serialized as a 4-byte integer denoting the number of
   * elements in the list, followed by the serialized elements.
   */
  deserializeNodeList(): Array<?HermesNode> {
    const size = this.next();
    const nodeList = [];

    for (let i = 0; i < size; i++) {
      nodeList.push(this.deserializeNode());
    }

    return nodeList;
  }

  /**
   * Comments are serialized as a node list, where each comment is serialized
   * as a 4-byte integer denoting comment type, followed by a 4-byte value
   * denoting the loc ID, followed by a serialized string for the comment value.
   */
  deserializeComments(): Array<HermesComment> {
    const size = this.next();
    const comments = [];

    for (let i = 0; i < size; i++) {
      const commentType = this.commentTypes[this.next()];
      const loc = this.addEmptyLoc();
      const value = this.deserializeString();
      comments.push({
        type: commentType,
        loc,
        value,
      });
    }

    return comments;
  }

  deserializeTokens(): Array<HermesToken> {
    const size = this.next();
    const tokens = [];

    for (let i = 0; i < size; i++) {
      const tokenType = this.tokenTypes[this.next()];
      const loc = this.addEmptyLoc();
      const value = this.deserializeString();
      tokens.push({
        type: tokenType,
        loc,
        value,
      });
    }

    return tokens;
  }

  /**
   * While deserializing the AST locations are represented by
   * a 4-byte loc ID. This is used to create a map of loc IDs to empty loc
   * objects that are filled after the AST has been deserialized.
   */
  addEmptyLoc(): HermesSourceLocation {
    const loc: HermesSourceLocation = {};
    this.locMap[this.next()] = loc;
    return loc;
  }

  /**
   * Positions are serialized as a loc ID which denotes which loc it is associated with,
   * followed by kind which denotes whether it is a start or end position,
   * followed by line, column, and offset (4-bytes each).
   */
  fillLocs(): void {
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
          column,
        };
        loc.rangeStart = offset;
      } else {
        loc.end = {
          line,
          column,
        };
        loc.rangeEnd = offset;
      }
    }
  }
}

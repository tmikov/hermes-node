'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.transformProgram = transformProgram;

var _SimpleTransform = require("../transform/SimpleTransform");

var _astNodeMutationHelpers = require("../transform/astNodeMutationHelpers");

var _createSyntaxError = require("../utils/createSyntaxError");

var _Builders = require("../utils/Builders");

var _GenID = _interopRequireDefault(require("../utils/GenID"));

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

let genID = null;

function genIdent() {
  if (genID == null) {
    throw Error('GenID must be initialized at the start of the transform.');
  }

  return (0, _Builders.ident)(genID.id());
}

function objKeyToString(node) {
  switch (node.type) {
    case 'Identifier':
      return node.name;

    case 'Literal':
      {
        const {
          value
        } = node;

        if (typeof value === 'number') {
          return String(value);
        } else if (typeof value === 'string') {
          return value;
        } else {
          return node.raw;
        }
      }
  }
}

function convertMemberPattern(pattern) {
  const {
    base,
    property,
    loc,
    range
  } = pattern;
  const object = base.type === 'MatchIdentifierPattern' ? base.id : convertMemberPattern(base);

  if (property.type === 'Identifier') {
    return {
      type: 'MemberExpression',
      object,
      property,
      computed: false,
      optional: false,
      ...(0, _Builders.etc)({
        loc,
        range
      })
    };
  } else {
    return {
      type: 'MemberExpression',
      object,
      property,
      computed: true,
      optional: false,
      ...(0, _Builders.etc)({
        loc,
        range
      })
    };
  }
}

function checkDuplicateBindingName(seenBindingNames, node, name) {
  if (seenBindingNames.has(name)) {
    throw (0, _createSyntaxError.createSyntaxError)(node, `Duplicate variable name '${name}' in match case pattern.`);
  }

  seenBindingNames.add(name);
}

function checkBindingKind(node, kind) {
  if (kind === 'var') {
    throw (0, _createSyntaxError.createSyntaxError)(node, `'var' bindings are not allowed. Use 'const' or 'let'.`);
  }
}

function needsPropExistsCond(pattern) {
  switch (pattern.type) {
    case 'MatchWildcardPattern':
    case 'MatchBindingPattern':
    case 'MatchIdentifierPattern':
    case 'MatchMemberPattern':
      return true;

    case 'MatchLiteralPattern':
    case 'MatchUnaryPattern':
    case 'MatchObjectPattern':
    case 'MatchInstancePattern':
    case 'MatchArrayPattern':
      return false;

    case 'MatchAsPattern':
      {
        const {
          pattern: asPattern
        } = pattern;
        return needsPropExistsCond(asPattern);
      }

    case 'MatchOrPattern':
      {
        const {
          patterns
        } = pattern;
        return patterns.some(needsPropExistsCond);
      }
  }
}

function analyzeProperties(key, pattern, seenBindingNames, properties, rest) {
  const conditions = [];
  const bindings = [];
  const objKeys = [];
  const seenNames = new Set();
  properties.forEach(prop => {
    const {
      key: objKey,
      pattern: propPattern
    } = prop;
    objKeys.push(objKey);
    const name = objKeyToString(objKey);

    if (seenNames.has(name)) {
      throw (0, _createSyntaxError.createSyntaxError)(propPattern, `Duplicate property name '${name}' in match object pattern.`);
    }

    seenNames.add(name);
    const propKey = key.concat(objKey);

    if (needsPropExistsCond(propPattern)) {
      conditions.push({
        type: 'prop-exists',
        key,
        propName: name
      });
    }

    const {
      conditions: childConditions,
      bindings: childBindings
    } = analyzePattern(propPattern, propKey, seenBindingNames);
    conditions.push(...childConditions);
    bindings.push(...childBindings);
  });

  if (rest != null && rest.argument != null) {
    const {
      id,
      kind
    } = rest.argument;
    checkDuplicateBindingName(seenBindingNames, rest.argument, id.name);
    checkBindingKind(pattern, kind);
    bindings.push({
      type: 'object-rest',
      key,
      exclude: objKeys,
      kind,
      id
    });
  }

  return {
    conditions,
    bindings
  };
}

function constructorExpression(constructor) {
  switch (constructor.type) {
    case 'MatchIdentifierPattern':
      return constructor.id;

    case 'MatchMemberPattern':
      return convertMemberPattern(constructor);
  }
}

function analyzePattern(pattern, key, seenBindingNames) {
  switch (pattern.type) {
    case 'MatchWildcardPattern':
      {
        return {
          conditions: [],
          bindings: []
        };
      }

    case 'MatchLiteralPattern':
      {
        const {
          literal
        } = pattern;
        const condition = {
          type: 'eq',
          key,
          arg: literal
        };
        return {
          conditions: [condition],
          bindings: []
        };
      }

    case 'MatchUnaryPattern':
      {
        const {
          operator,
          argument,
          loc,
          range
        } = pattern;

        if (argument.value === 0) {
          throw (0, _createSyntaxError.createSyntaxError)(pattern, `'+0' and '-0' are not yet supported in match unary patterns.`);
        }

        const arg = {
          type: 'UnaryExpression',
          operator,
          argument,
          prefix: true,
          ...(0, _Builders.etc)({
            loc,
            range
          })
        };
        const condition = {
          type: 'eq',
          key,
          arg
        };
        return {
          conditions: [condition],
          bindings: []
        };
      }

    case 'MatchIdentifierPattern':
      {
        const {
          id
        } = pattern;
        const condition = id.name === 'NaN' ? {
          type: 'is-nan',
          key
        } : {
          type: 'eq',
          key,
          arg: id
        };
        return {
          conditions: [condition],
          bindings: []
        };
      }

    case 'MatchMemberPattern':
      {
        const arg = convertMemberPattern(pattern);
        const condition = {
          type: 'eq',
          key,
          arg
        };
        return {
          conditions: [condition],
          bindings: []
        };
      }

    case 'MatchBindingPattern':
      {
        const {
          id,
          kind
        } = pattern;
        checkDuplicateBindingName(seenBindingNames, pattern, id.name);
        checkBindingKind(pattern, kind);
        const binding = {
          type: 'id',
          key,
          kind,
          id
        };
        return {
          conditions: [],
          bindings: [binding]
        };
      }

    case 'MatchAsPattern':
      {
        const {
          pattern: asPattern,
          target
        } = pattern;

        if (asPattern.type === 'MatchBindingPattern') {
          throw (0, _createSyntaxError.createSyntaxError)(pattern, `Match 'as' patterns are not allowed directly on binding patterns.`);
        }

        const {
          conditions,
          bindings
        } = analyzePattern(asPattern, key, seenBindingNames);
        const constKind = 'const';
        const [id, kind] = target.type === 'MatchBindingPattern' ? [target.id, target.kind] : [target, constKind];
        checkDuplicateBindingName(seenBindingNames, pattern, id.name);
        checkBindingKind(pattern, kind);
        const binding = {
          type: 'id',
          key,
          kind,
          id
        };
        return {
          conditions,
          bindings: bindings.concat(binding)
        };
      }

    case 'MatchArrayPattern':
      {
        const {
          elements,
          rest
        } = pattern;
        const lengthOp = rest == null ? 'eq' : 'gte';
        const conditions = [{
          type: 'array',
          key,
          length: elements.length,
          lengthOp
        }];
        const bindings = [];
        elements.forEach((element, i) => {
          const elementKey = key.concat((0, _Builders.numberLiteral)(i));
          const {
            conditions: childConditions,
            bindings: childBindings
          } = analyzePattern(element, elementKey, seenBindingNames);
          conditions.push(...childConditions);
          bindings.push(...childBindings);
        });

        if (rest != null && rest.argument != null) {
          const {
            id,
            kind
          } = rest.argument;
          checkDuplicateBindingName(seenBindingNames, rest.argument, id.name);
          checkBindingKind(pattern, kind);
          bindings.push({
            type: 'array-rest',
            key,
            exclude: elements.length,
            kind,
            id
          });
        }

        return {
          conditions,
          bindings
        };
      }

    case 'MatchObjectPattern':
      {
        const {
          properties,
          rest
        } = pattern;
        const {
          conditions: propertyConditions,
          bindings
        } = analyzeProperties(key, pattern, seenBindingNames, properties, rest);
        const conditions = [{
          type: 'object',
          key
        }, ...propertyConditions];
        return {
          conditions,
          bindings
        };
      }

    case 'MatchInstancePattern':
      {
        const {
          targetConstructor,
          properties: {
            properties,
            rest
          }
        } = pattern;
        const {
          conditions: propertyConditions,
          bindings
        } = analyzeProperties(key, pattern, seenBindingNames, properties, rest);
        const conditions = [{
          type: 'instanceof',
          key,
          constructor: constructorExpression(targetConstructor)
        }, ...propertyConditions];
        return {
          conditions,
          bindings
        };
      }

    case 'MatchOrPattern':
      {
        const {
          patterns
        } = pattern;
        let hasWildcard = false;
        const orConditions = patterns.map(subpattern => {
          const {
            conditions,
            bindings
          } = analyzePattern(subpattern, key, seenBindingNames);

          if (bindings.length > 0) {
            throw (0, _createSyntaxError.createSyntaxError)(pattern, `Bindings in match 'or' patterns are not yet supported.`);
          }

          if (conditions.length === 0) {
            hasWildcard = true;
          }

          return conditions;
        });

        if (hasWildcard) {
          return {
            conditions: [],
            bindings: []
          };
        }

        return {
          conditions: [{
            type: 'or',
            orConditions
          }],
          bindings: []
        };
      }
  }
}

function expressionOfKey(root, key) {
  return key.reduce((acc, prop) => prop.type === 'Identifier' ? {
    type: 'MemberExpression',
    object: acc,
    property: (0, _astNodeMutationHelpers.shallowCloneNode)(prop),
    computed: false,
    optional: false,
    ...(0, _Builders.etc)()
  } : {
    type: 'MemberExpression',
    object: acc,
    property: (0, _astNodeMutationHelpers.shallowCloneNode)(prop),
    computed: true,
    optional: false,
    ...(0, _Builders.etc)()
  }, (0, _astNodeMutationHelpers.deepCloneNode)(root));
}

function testsOfCondition(root, condition) {
  switch (condition.type) {
    case 'eq':
      {
        const {
          key,
          arg
        } = condition;
        return [{
          type: 'BinaryExpression',
          left: expressionOfKey(root, key),
          right: arg,
          operator: '===',
          ...(0, _Builders.etc)()
        }];
      }

    case 'is-nan':
      {
        const {
          key
        } = condition;
        const callee = {
          type: 'MemberExpression',
          object: (0, _Builders.ident)('Number'),
          property: (0, _Builders.ident)('isNaN'),
          computed: false,
          optional: false,
          ...(0, _Builders.etc)()
        };
        return [(0, _Builders.callExpression)(callee, [expressionOfKey(root, key)])];
      }

    case 'array':
      {
        const {
          key,
          length,
          lengthOp
        } = condition;
        const operator = lengthOp === 'eq' ? '===' : '>=';
        const isArray = (0, _Builders.callExpression)({
          type: 'MemberExpression',
          object: (0, _Builders.ident)('Array'),
          property: (0, _Builders.ident)('isArray'),
          computed: false,
          optional: false,
          ...(0, _Builders.etc)()
        }, [expressionOfKey(root, key)]);
        const lengthCheck = {
          type: 'BinaryExpression',
          left: {
            type: 'MemberExpression',
            object: expressionOfKey(root, key),
            property: (0, _Builders.ident)('length'),
            computed: false,
            optional: false,
            ...(0, _Builders.etc)()
          },
          right: (0, _Builders.numberLiteral)(length),
          operator,
          ...(0, _Builders.etc)()
        };
        return [isArray, lengthCheck];
      }

    case 'object':
      {
        const {
          key
        } = condition;
        const typeofObject = (0, _Builders.typeofExpression)(expressionOfKey(root, key), 'object');
        const typeofFunction = (0, _Builders.typeofExpression)(expressionOfKey(root, key), 'function');
        const notNull = {
          type: 'BinaryExpression',
          left: expressionOfKey(root, key),
          right: (0, _Builders.nullLiteral)(),
          operator: '!==',
          ...(0, _Builders.etc)()
        };
        return [(0, _Builders.disjunction)([(0, _Builders.conjunction)([typeofObject, notNull]), typeofFunction])];
      }

    case 'instanceof':
      {
        const {
          key,
          constructor
        } = condition;
        return [{
          type: 'BinaryExpression',
          left: expressionOfKey(root, key),
          right: constructor,
          operator: 'instanceof',
          ...(0, _Builders.etc)()
        }];
      }

    case 'prop-exists':
      {
        const {
          key,
          propName
        } = condition;
        const inObject = {
          type: 'BinaryExpression',
          left: (0, _Builders.stringLiteral)(propName),
          right: expressionOfKey(root, key),
          operator: 'in',
          ...(0, _Builders.etc)()
        };
        return [inObject];
      }

    case 'or':
      {
        const {
          orConditions
        } = condition;
        const tests = orConditions.map(conditions => (0, _Builders.conjunction)(testsOfConditions(root, conditions)));
        return [(0, _Builders.disjunction)(tests)];
      }
  }
}

function testsOfConditions(root, conditions) {
  return conditions.flatMap(condition => testsOfCondition(root, condition));
}

function statementsOfBindings(root, bindings) {
  return bindings.map(binding => {
    switch (binding.type) {
      case 'id':
        {
          const {
            key,
            kind,
            id
          } = binding;
          return (0, _Builders.variableDeclaration)(kind, id, expressionOfKey(root, key));
        }

      case 'array-rest':
        {
          const {
            key,
            kind,
            id,
            exclude
          } = binding;
          const init = (0, _Builders.callExpression)({
            type: 'MemberExpression',
            object: expressionOfKey(root, key),
            property: (0, _Builders.ident)('slice'),
            computed: false,
            optional: false,
            ...(0, _Builders.etc)()
          }, [(0, _Builders.numberLiteral)(exclude)]);
          return (0, _Builders.variableDeclaration)(kind, id, init);
        }

      case 'object-rest':
        {
          const {
            key,
            kind,
            id,
            exclude
          } = binding;
          const destructuring = {
            type: 'ObjectPattern',
            properties: exclude.map(prop => prop.type === 'Identifier' ? {
              type: 'Property',
              key: (0, _astNodeMutationHelpers.shallowCloneNode)(prop),
              value: genIdent(),
              kind: 'init',
              computed: false,
              method: false,
              shorthand: false,
              ...(0, _Builders.etc)(),
              parent: _Builders.EMPTY_PARENT
            } : {
              type: 'Property',
              key: (0, _astNodeMutationHelpers.shallowCloneNode)(prop),
              value: genIdent(),
              kind: 'init',
              computed: true,
              method: false,
              shorthand: false,
              ...(0, _Builders.etc)(),
              parent: _Builders.EMPTY_PARENT
            }).concat({
              type: 'RestElement',
              argument: id,
              ...(0, _Builders.etc)()
            }),
            typeAnnotation: null,
            ...(0, _Builders.etc)()
          };
          return (0, _Builders.variableDeclaration)(kind, destructuring, expressionOfKey(root, key));
        }
    }
  });
}

const fallthroughErrorMsgText = `Match: No case succesfully matched. Make exhaustive or add a wildcard case using '_'.`;

function fallthroughErrorMsg(value) {
  return {
    type: 'BinaryExpression',
    operator: '+',
    left: (0, _Builders.stringLiteral)(`${fallthroughErrorMsgText} Argument: `),
    right: value,
    ...(0, _Builders.etc)()
  };
}

function fallthroughError(value) {
  return (0, _Builders.throwStatement)(fallthroughErrorMsg(value));
}

function calculateSimpleArgument(node) {
  switch (node.type) {
    case 'Identifier':
    case 'Super':
      return true;

    case 'MemberExpression':
      {
        const {
          object,
          property,
          computed
        } = node;

        if (computed && property.type !== 'Literal') {
          return false;
        }

        return calculateSimpleArgument(object);
      }

    default:
      return false;
  }
}

function analyzeCases(cases) {
  let hasBindings = false;
  let hasWildcard = false;
  const analyses = [];

  for (let i = 0; i < cases.length; i++) {
    const {
      pattern,
      guard,
      body
    } = cases[i];
    const {
      conditions,
      bindings
    } = analyzePattern(pattern, [], new Set());
    hasBindings = hasBindings || bindings.length > 0;
    analyses.push({
      conditions,
      bindings,
      guard,
      body
    });

    if (conditions.length === 0 && guard == null) {
      hasWildcard = true;
      break;
    }
  }

  return {
    hasBindings,
    hasWildcard,
    analyses
  };
}

function mapMatchExpression(node) {
  const {
    argument,
    cases
  } = node;
  const {
    hasBindings,
    hasWildcard,
    analyses
  } = analyzeCases(cases);
  const isSimpleArgument = !hasBindings && calculateSimpleArgument(argument);
  const genRoot = !isSimpleArgument ? genIdent() : null;
  const root = genRoot == null ? argument : genRoot;

  if (isSimpleArgument) {
    const wildcardAnalaysis = hasWildcard ? analyses.pop() : null;
    const lastBody = wildcardAnalaysis != null ? wildcardAnalaysis.body : (0, _Builders.iife)([fallthroughError((0, _astNodeMutationHelpers.shallowCloneNode)(root))]);
    return analyses.reverse().reduce((acc, analysis) => {
      const {
        conditions,
        guard,
        body
      } = analysis;
      const tests = testsOfConditions(root, conditions);

      if (guard != null) {
        tests.push(guard);
      }

      return {
        type: 'ConditionalExpression',
        test: (0, _Builders.conjunction)(tests),
        consequent: body,
        alternate: acc,
        ...(0, _Builders.etc)()
      };
    }, lastBody);
  }

  const statements = analyses.map(({
    conditions,
    bindings,
    guard,
    body
  }) => {
    const returnNode = {
      type: 'ReturnStatement',
      argument: body,
      ...(0, _Builders.etc)()
    };
    const bodyNode = guard == null ? returnNode : {
      type: 'IfStatement',
      test: guard,
      consequent: returnNode,
      ...(0, _Builders.etc)()
    };
    const bindingNodes = statementsOfBindings(root, bindings);
    const caseBody = bindingNodes.concat(bodyNode);

    if (conditions.length > 0) {
      const tests = testsOfConditions(root, conditions);
      return {
        type: 'IfStatement',
        test: (0, _Builders.conjunction)(tests),
        consequent: {
          type: 'BlockStatement',
          body: caseBody,
          ...(0, _Builders.etc)()
        },
        ...(0, _Builders.etc)()
      };
    } else {
      if (bindingNodes.length > 0) {
        return {
          type: 'BlockStatement',
          body: caseBody,
          ...(0, _Builders.etc)()
        };
      } else {
        return bodyNode;
      }
    }
  });

  if (!hasWildcard) {
    statements.push(fallthroughError((0, _astNodeMutationHelpers.shallowCloneNode)(root)));
  }

  const [params, args] = genRoot == null ? [[], []] : [[genRoot], [argument]];
  return (0, _Builders.iife)(statements, params, args);
}

function mapMatchStatement(node) {
  const {
    argument,
    cases
  } = node;
  const {
    hasBindings,
    hasWildcard,
    analyses
  } = analyzeCases(cases);
  const topLabel = genIdent();
  const isSimpleArgument = !hasBindings && calculateSimpleArgument(argument);
  const genRoot = !isSimpleArgument ? genIdent() : null;
  const root = genRoot == null ? argument : genRoot;
  const statements = [];

  if (genRoot != null) {
    statements.push((0, _Builders.variableDeclaration)('const', genRoot, argument));
  }

  analyses.forEach(({
    conditions,
    bindings,
    guard,
    body
  }) => {
    const breakNode = {
      type: 'BreakStatement',
      label: (0, _astNodeMutationHelpers.shallowCloneNode)(topLabel),
      ...(0, _Builders.etc)()
    };
    const bodyStatements = body.body.concat(breakNode);
    const guardedBodyStatements = guard == null ? bodyStatements : [{
      type: 'IfStatement',
      test: guard,
      consequent: {
        type: 'BlockStatement',
        body: bodyStatements,
        ...(0, _Builders.etc)()
      },
      ...(0, _Builders.etc)()
    }];
    const bindingNodes = statementsOfBindings(root, bindings);
    const caseBody = bindingNodes.concat(guardedBodyStatements);

    if (conditions.length > 0) {
      const tests = testsOfConditions(root, conditions);
      statements.push({
        type: 'IfStatement',
        test: (0, _Builders.conjunction)(tests),
        consequent: {
          type: 'BlockStatement',
          body: caseBody,
          ...(0, _Builders.etc)()
        },
        ...(0, _Builders.etc)()
      });
    } else {
      statements.push({
        type: 'BlockStatement',
        body: caseBody,
        ...(0, _Builders.etc)()
      });
    }
  });

  if (!hasWildcard) {
    statements.push(fallthroughError((0, _astNodeMutationHelpers.shallowCloneNode)(root)));
  }

  return {
    type: 'LabeledStatement',
    label: topLabel,
    body: {
      type: 'BlockStatement',
      body: statements,
      ...(0, _Builders.etc)()
    },
    ...(0, _Builders.etc)()
  };
}

function transformProgram(program, _options) {
  genID = new _GenID.default('m');
  return _SimpleTransform.SimpleTransform.transformProgram(program, {
    transform(node) {
      switch (node.type) {
        case 'MatchExpression':
          {
            return mapMatchExpression(node);
          }

        case 'MatchStatement':
          {
            return mapMatchStatement(node);
          }

        case 'Identifier':
          {
            if (genID == null) {
              throw Error('GenID must be initialized at the start of the transform.');
            }

            genID.addUsage(node.name);
            return node;
          }

        default:
          {
            return node;
          }
      }
    }

  });
}
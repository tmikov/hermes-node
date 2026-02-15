'use strict';
exports.name = 'circ_b';
// This circular require gets circ_a's partial exports (only 'name' set so far).
var circA = require('circ_a');
exports.a_name = circA.name;

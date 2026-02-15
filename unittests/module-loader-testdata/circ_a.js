'use strict';
exports.name = 'circ_a';
// At this point, circ_b's require of circ_a will see our partially populated exports.
var circB = require('circ_b');
exports.b_name = circB.name;

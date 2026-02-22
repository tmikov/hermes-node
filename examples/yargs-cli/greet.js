// A small CLI tool built with yargs to demonstrate npm package resolution
// with a real transitive dependency tree (16 packages).
//
// Usage:
//   hermes-node greet.js hello --name World --excited
//   hermes-node greet.js count --from 1 --to 5
//   hermes-node greet.js --help

'use strict';

var yargs = require('yargs/yargs');
var helpers = require('yargs/helpers');

var argv = yargs(helpers.hideBin(process.argv))
  .scriptName('greet')
  .usage('$0 <command> [options]')
  .command('hello', 'Say hello to someone', function(yargs) {
    return yargs
      .option('name', {
        alias: 'n',
        type: 'string',
        default: 'World',
        describe: 'Who to greet',
      })
      .option('excited', {
        alias: 'e',
        type: 'boolean',
        default: false,
        describe: 'Add excitement',
      })
      .option('repeat', {
        alias: 'r',
        type: 'number',
        default: 1,
        describe: 'How many times to greet',
      });
  }, function(argv) {
    var punct = argv.excited ? '!!!' : '.';
    for (var i = 0; i < argv.repeat; i++) {
      console.log('Hello, ' + argv.name + punct);
    }
  })
  .command('count', 'Count numbers', function(yargs) {
    return yargs
      .option('from', {
        type: 'number',
        default: 1,
        describe: 'Start number',
      })
      .option('to', {
        type: 'number',
        default: 10,
        describe: 'End number',
      })
      .option('step', {
        type: 'number',
        default: 1,
        describe: 'Step size',
      });
  }, function(argv) {
    var nums = [];
    for (var i = argv.from; i <= argv.to; i += argv.step) {
      nums.push(i);
    }
    console.log(nums.join(', '));
  })
  .demandCommand(1, 'Please specify a command')
  .strict()
  .help()
  .argv;

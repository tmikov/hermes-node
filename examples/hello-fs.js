var fs = require('fs');
var path = require('path');

var tmp = fs.mkdtempSync('/tmp/hermes-demo-');
console.log('Created temp dir:', tmp);

// Write a JSON file.
var data = { name: 'hermes-node', features: ['fs', 'console', 'streams', 'timers'] };
var file = path.join(tmp, 'data.json');
fs.writeFileSync(file, JSON.stringify(data, null, 2));
console.log('Wrote:', file);

// Read it back and parse.
var contents = fs.readFileSync(file, 'utf8');
var parsed = JSON.parse(contents);
console.log('Read back:', parsed);

// List directory.
console.log('Directory contents:', fs.readdirSync(tmp));

// File stats.
var stat = fs.statSync(file);
console.log('Size: %d bytes, modified: %s', stat.size, stat.mtime);

// Cleanup.
fs.rmSync(tmp, { recursive: true });
console.log('Cleaned up.');

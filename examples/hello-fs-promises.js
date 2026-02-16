var fs = require('fs/promises');
var path = require('path');

async function main() {
  var tmp = await fs.mkdtemp('/tmp/hermes-demo-');
  console.log('Created temp dir:', tmp);

  var data = { name: 'hermes-node', features: ['fs', 'console', 'streams', 'timers'] };
  var file = path.join(tmp, 'data.json');

  await fs.writeFile(file, JSON.stringify(data, null, 2));
  console.log('Wrote:', file);

  var contents = await fs.readFile(file, 'utf8');
  console.log('Read back:', JSON.parse(contents));

  console.log('Directory contents:', await fs.readdir(tmp));

  var stat = await fs.stat(file);
  console.log('Size: %d bytes, modified: %s', stat.size, stat.mtime);

  await fs.rm(tmp, { recursive: true });
  console.log('Cleaned up.');
}

main();

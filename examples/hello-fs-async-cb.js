var fs = require('fs');
var path = require('path');

fs.mkdtemp('/tmp/hermes-demo-', function(err, tmp) {
  if (err) throw err;
  console.log('Created temp dir:', tmp);

  var data = { name: 'hermes-node', features: ['fs', 'console', 'streams', 'timers'] };
  var file = path.join(tmp, 'data.json');

  fs.writeFile(file, JSON.stringify(data, null, 2), function(err) {
    if (err) throw err;
    console.log('Wrote:', file);

    fs.readFile(file, 'utf8', function(err, contents) {
      if (err) throw err;
      console.log('Read back:', JSON.parse(contents));

      fs.readdir(tmp, function(err, files) {
        if (err) throw err;
        console.log('Directory contents:', files);

        fs.stat(file, function(err, stat) {
          if (err) throw err;
          console.log('Size: %d bytes, modified: %s', stat.size, stat.mtime);

          fs.rm(tmp, { recursive: true }, function(err) {
            if (err) throw err;
            console.log('Cleaned up.');
          });
        });
      });
    });
  });
});

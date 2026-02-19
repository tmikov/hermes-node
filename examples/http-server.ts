// An HTTP server in TypeScript that also uses the file system.
// Usage: hermes-node http-server.ts [port]
//
// Routes:
//   GET /             - welcome page
//   GET /json         - JSON response with timestamp
//   GET /file?path=X  - serve a file from disk
//   GET /ls?dir=X     - list a directory (defaults to cwd)

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

interface Route {
  method: string;
  path: string;
  handler: (req: any, res: any, query: Record<string, string>) => void;
}

function parseQuery(url: string): {pathname: string; query: Record<string, string>} {
  const qIdx: number = url.indexOf('?');
  if (qIdx === -1) return {pathname: url, query: {}};
  const pathname: string = url.slice(0, qIdx);
  const query: Record<string, string> = {};
  for (const pair of url.slice(qIdx + 1).split('&')) {
    const eq: number = pair.indexOf('=');
    if (eq === -1) {
      query[decodeURIComponent(pair)] = '';
    } else {
      query[decodeURIComponent(pair.slice(0, eq))] = decodeURIComponent(pair.slice(eq + 1));
    }
  }
  return {pathname, query};
}

const routes: Route[] = [
  {
    method: 'GET',
    path: '/',
    handler(_req: any, res: any): void {
      res.writeHead(200, {'Content-Type': 'text/html'});
      res.end(
        '<h1>Hello from hermes-node!</h1>\n' +
          '<ul>\n' +
          '  <li><a href="/json">/json</a> - JSON with timestamp</li>\n' +
          '  <li><a href="/ls">/ls?dir=.</a> - list a directory</li>\n' +
          '  <li><a href="/file?path=examples/http-server.ts">/file?path=...</a> - serve a file</li>\n' +
          '</ul>\n',
      );
    },
  },
  {
    method: 'GET',
    path: '/json',
    handler(_req: any, res: any): void {
      const data: object = {message: 'Hello, World!', ts: Date.now()};
      res.writeHead(200, {'Content-Type': 'application/json'});
      res.end(JSON.stringify(data) + '\n');
    },
  },
  {
    method: 'GET',
    path: '/file',
    handler(_req: any, res: any, query: Record<string, string>): void {
      const filePath: string = query.path || '';
      if (!filePath) {
        res.writeHead(400, {'Content-Type': 'text/plain'});
        res.end('Missing ?path= parameter\n');
        return;
      }
      const resolved: string = path.resolve(filePath);
      fs.readFile(resolved, (err: any, data: Buffer): void => {
        if (err) {
          const status: number = err.code === 'ENOENT' ? 404 : 500;
          res.writeHead(status, {'Content-Type': 'text/plain'});
          res.end(err.code + ': ' + resolved + '\n');
          return;
        }
        res.writeHead(200, {'Content-Type': 'text/plain; charset=utf-8'});
        res.end(data);
      });
    },
  },
  {
    method: 'GET',
    path: '/ls',
    handler(_req: any, res: any, query: Record<string, string>): void {
      const dir: string = path.resolve(query.dir || '.');
      fs.readdir(dir, {withFileTypes: true}, (err: any, entries: any[]): void => {
        if (err) {
          const status: number = err.code === 'ENOENT' ? 404 : 500;
          res.writeHead(status, {'Content-Type': 'text/plain'});
          res.end(err.code + ': ' + dir + '\n');
          return;
        }
        const items: object[] = entries.map((e: any): object => ({
          name: e.name,
          type: e.isDirectory() ? 'dir' : e.isFile() ? 'file' : 'other',
        }));
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({dir, entries: items}, null, 2) + '\n');
      });
    },
  },
];

function findRoute(method: string, pathname: string): Route | undefined {
  for (const route of routes) {
    if (route.method === method && route.path === pathname)
      return route;
  }
  return undefined;
}

const port: number = parseInt(process.argv[2], 10) || 3000;

const server = http.createServer((req: any, res: any): void => {
  const {pathname, query} = parseQuery(req.url);
  const route: Route | undefined = findRoute(req.method, pathname);
  if (route) {
    console.log('%s %s -> matched', req.method, req.url);
    route.handler(req, res, query);
  } else {
    console.log('%s %s -> 404', req.method, req.url);
    res.writeHead(404, {'Content-Type': 'text/plain'});
    res.end('Not found\n');
  }
});

server.listen(port, (): void => {
  console.log('Listening on http://localhost:' + port + '/');
});

// Static server for the benchmark pages: Range requests + COOP/COEP (needed for SharedArrayBuffer).
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
const ROOT = path.resolve(process.argv[2] || ".");
const PORT = +(process.argv[3] || 8766);
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm", ".json": "application/json", ".css": "text/css" };
http.createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (req.method === "PUT" && url.startsWith("/uploads/")) {          // used by prep-v86.html to save the prepared disk
    const dest = path.join(ROOT, url); fs.mkdirSync(path.dirname(dest), { recursive: true });
    const ws = fs.createWriteStream(dest); req.pipe(ws);
    ws.on("finish", () => { res.writeHead(200); res.end("saved " + fs.statSync(dest).size); });
    return;
  }
  const file = path.join(ROOT, url.endsWith("/") ? url + "index.html" : url);
  if (!file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); return res.end(); }
  const size = fs.statSync(file).size;
  const head = { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream", "Accept-Ranges": "bytes",
    "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp", "Cache-Control": "no-store" };
  const m = /bytes=(\d+)-(\d*)/.exec(req.headers.range || "");
  if (m) {
    const start = +m[1], end = m[2] ? +m[2] : size - 1;
    res.writeHead(206, { ...head, "Content-Range": `bytes ${start}-${end}/${size}`, "Content-Length": end - start + 1 });
    return fs.createReadStream(file, { start, end }).pipe(res);
  }
  res.writeHead(200, { ...head, "Content-Length": size });
  fs.createReadStream(file).pipe(res);
}).listen(PORT, () => console.log(`serving ${ROOT} on http://localhost:${PORT}`));

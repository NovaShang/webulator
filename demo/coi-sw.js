// GitHub Pages cannot send COOP/COEP headers, and Webulator needs SharedArrayBuffer (cross-origin isolation).
// This service worker adds the headers to every same-origin response. index.html registers it and reloads once.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));

self.addEventListener("fetch", e => {
  const req = e.request;
  if (req.cache === "only-if-cached" && req.mode !== "same-origin") return;
  if (new URL(req.url).origin !== self.location.origin) return;
  e.respondWith(fetch(req).then(res => {
    if (res.status === 0) return res;
    const headers = new Headers(res.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    headers.set("Cross-Origin-Resource-Policy", "same-origin");
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
  }));
});

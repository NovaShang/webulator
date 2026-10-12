// CPU-profiles every dedicated worker of the first page over raw CDP (Playwright can't route worker sessions).
// The browser must be launched with --remote-debugging-port=<port>.
export async function startWorkerProfiles(port) {
  const { webSocketDebuggerUrl } = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  const ws = new WebSocket(webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0; const pending = new Map(), workers = [];
  const send = (method, params = {}, sessionId) => new Promise(r => {
    pending.set(++id, r); ws.send(JSON.stringify({ id, method, params, sessionId }));
  });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result ?? m.error); pending.delete(m.id); }
    if (m.method === "Target.attachedToTarget" && m.params.targetInfo.type === "worker") workers.push(m.params);
  };
  const { targetInfos } = await send("Target.getTargets");
  const pageT = targetInfos.find(t => t.type === "page" && t.url.includes("conformance"));
  const { sessionId: ps } = await send("Target.attachToTarget", { targetId: pageT.targetId, flatten: true });
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, ps);
  await new Promise(r => setTimeout(r, 500));
  for (const w of workers) {
    await send("Profiler.enable", {}, w.sessionId);
    await send("Profiler.setSamplingInterval", { interval: 200 }, w.sessionId);
    await send("Profiler.start", {}, w.sessionId);
  }
  return async function stop() {
    const out = [];
    for (const w of workers) out.push({ url: w.targetInfo.url, profile: (await send("Profiler.stop", {}, w.sessionId)).profile });
    ws.close();
    return out;
  };
}

// Self time by function, top n.
export function summarize(profile, n = 40) {
  const byId = new Map(profile.nodes.map(x => [x.id, x]));
  const self = new Map();
  const dts = profile.timeDeltas; let total = 0;
  profile.samples.forEach((sid, i) => {
    const node = byId.get(sid), cf = node.callFrame, dt = dts[i + 1] ?? 0;
    const file = (cf.url || "").split("/").pop();
    const key = `${cf.functionName || "(anon)"} ${file}${cf.lineNumber >= 0 && file ? ":" + (cf.lineNumber + 1) : ""}`;
    self.set(key, (self.get(key) ?? 0) + dt); total += dt;
  });
  const rows = [...self].sort((a, b) => b[1] - a[1]).slice(0, n);
  return { total_ms: total / 1000, rows: rows.map(([k, us]) => `${(100 * us / total).toFixed(1).padStart(5)}%  ${(us / 1000).toFixed(0).padStart(6)}ms  ${k}`) };
}

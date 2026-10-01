// Windows 98 on v86. Screen: 640×480, teal desktop, grey taskbar.
const near = (p, q, t = 12) => p.every((v, i) => Math.abs(v - q[i]) <= t);
const at = (img, x, y) => { const i = (y * img.width + x) * 4; return [img.data[i], img.data[i + 1], img.data[i + 2]]; };
const TEAL = [87, 168, 168], GREY = [192, 199, 200];

export default {
  bootTimeout: 120000,
  settle: 3000,
  busyAt: 4000,
  ready: img => img.width === 640 && near(at(img, 320, 466), GREY) && near(at(img, 400, 200), TEAL),
  // Start menu opens, closes with Escape.
  async alive(m, h) {
    const t = {};
    const menu = { x: 0, y: 200, width: 200, height: 250 };
    const before = await m.screen.read(menu);
    let t0 = performance.now();
    await h.click(m, 20, 466);
    await h.waitChange(m, before, menu, 500, 1000); t.start_menu_ms = +(performance.now() - t0).toFixed(1);
    const open = await m.screen.read(menu);
    t0 = performance.now();
    m.input.key("Escape", true); m.input.key("Escape", false);
    await h.waitChange(m, open, menu, 500, 1000); t.menu_close_ms = +(performance.now() - t0).toFixed(1);
    return t;
  },
  cursor: { area: { x: 220, y: 60, width: 380, height: 360 }, park: [600, 40], offset: [0, 0] },
  textEcho: {
    // Notepad does not wrap by default: break the 95 printable characters into lines of 32.
    text: Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i)).join("").match(/.{1,32}/g).join("\n"),
    async open(m, h) {
      m.input.key("ControlLeft", true); m.input.key("Escape", true); m.input.key("Escape", false); m.input.key("ControlLeft", false);
      await h.sleep(800);
      await m.input.type("r"); await h.sleep(1200);
      await m.input.type("notepad\n"); await h.sleep(2500); await h.settle(m, 800);
      return { x: 4, y: 40, width: 560, height: 120 };
    },
  },
  async shutdown(m, h) {
    m.input.key("ControlLeft", true); m.input.key("Escape", true); m.input.key("Escape", false); m.input.key("ControlLeft", false);
    await h.sleep(1000);
    await m.input.type("u"); await h.sleep(2000);
    m.input.key("Enter", true); m.input.key("Enter", false);
  },
};

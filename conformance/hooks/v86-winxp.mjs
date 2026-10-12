// Windows XP on v86 (spike S3). Only what the smoke/explore suites need for now.
const at = (img, x, y) => { const i = (y * img.width + x) * 4; return [img.data[i], img.data[i + 1], img.data[i + 2]]; };
export default {
  bootTimeout: 600000,
  settle: 5000,
  busyAt: 8000,
  // The Luna taskbar along the bottom edge with the green Start button at its left end (the logon screen has a
  // similar blue strip, but no Start button).
  ready: img => {
    if (img.width < 640) return false;
    const [r, g, b] = at(img, img.width >> 1, img.height - 10), [sr, sg, sb] = at(img, 85, img.height - 10);
    return b > 150 && r < 80 && sg > sr + 40 && sg > sb + 40;
  },
  // Start menu opens (click), closes with Escape; Run dialog opens Notepad.
  async alive(m, h) {
    const t = {};
    const menu = { x: 0, y: m.screen.height - 500, width: 380, height: 460 };
    const before = await m.screen.read(menu);
    let t0 = performance.now();
    await h.click(m, 40, m.screen.height - 12);
    await h.waitChange(m, before, menu, 2000, 5000); t.start_menu_ms = +(performance.now() - t0).toFixed(1);
    const open = await m.screen.read(menu);
    m.input.key("Escape", true); m.input.key("Escape", false);
    await h.waitChange(m, open, menu, 2000, 5000);
    await h.sleep(500);
    m.input.key("MetaLeft", true); m.input.key("KeyR", true); m.input.key("KeyR", false); m.input.key("MetaLeft", false);
    await h.sleep(1500);
    const center = { x: 100, y: 60, width: 700, height: 500 }, b2 = await m.screen.read(center);
    t0 = performance.now();
    await m.input.type("notepad\n");
    await h.waitChange(m, b2, center, 5000, 10000); t.notepad_ms = +(performance.now() - t0).toFixed(1);
    return t;
  },
  // Bake: close the Start menu that XP opens at a user's first logon, so the saved profile no longer counts as new.
  async bakePrepare(m, h) {
    m.input.key("Escape", true); m.input.key("Escape", false); await h.sleep(2000);
  },
  // VMware's mouse driver switches to absolute mode only after the first mouse movement.
  async afterReady(m, h) {
    m.input.pointer.moveTo(500, 400); await h.sleep(300); m.input.pointer.moveTo(520, 420); await h.sleep(1500);
  },
  async shutdown(m, h) {
    m.input.key("ControlLeft", true); m.input.key("Escape", true); m.input.key("Escape", false); m.input.key("ControlLeft", false);
    await h.sleep(3000); await m.input.type("u"); await h.sleep(4000); await m.input.type("u");
  },
  textEcho: {
    // Notepad does not wrap by default: break the 95 printable characters into lines of 32.
    text: Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i)).join("").match(/.{1,32}/g).join("\n"),
    async open(m, h) {
      m.input.key("MetaLeft", true); m.input.key("KeyR", true); m.input.key("KeyR", false); m.input.key("MetaLeft", false);
      await h.sleep(1500);
      await m.input.type("notepad\n"); await h.sleep(4000); await h.settle(m, 1000);
      return { x: 136, y: 224, width: 740, height: 60 };
    },
  },
  cursor: { area: { x: 200, y: 100, width: 400, height: 300 }, park: [1000, 20], offset: [0, -1] },
};

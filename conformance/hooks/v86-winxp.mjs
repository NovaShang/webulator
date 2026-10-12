// Windows XP on v86 (spike S3). Only what the smoke/explore suites need for now.
const at = (img, x, y) => { const i = (y * img.width + x) * 4; return [img.data[i], img.data[i + 1], img.data[i + 2]]; };
export default {
  bootTimeout: 600000,
  settle: 5000,
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
    const menu = { x: 0, y: 150, width: 380, height: 300 };
    const before = await m.screen.read(menu);
    let t0 = performance.now();
    // Keyboard, not the pointer: XP has no absolute-pointer driver yet (VMware mouse driver pending).
    m.input.key("ControlLeft", true); m.input.key("Escape", true); m.input.key("Escape", false); m.input.key("ControlLeft", false);
    await h.waitChange(m, before, menu, 2000, 5000); t.start_menu_ms = +(performance.now() - t0).toFixed(1);
    const open = await m.screen.read(menu);
    m.input.key("Escape", true); m.input.key("Escape", false);
    await h.waitChange(m, open, menu, 2000, 5000);
    await h.sleep(500);
    m.input.key("MetaLeft", true); m.input.key("KeyR", true); m.input.key("KeyR", false); m.input.key("MetaLeft", false);
    await h.sleep(1500);
    const center = { x: 100, y: 60, width: 440, height: 300 }, b2 = await m.screen.read(center);
    t0 = performance.now();
    await m.input.type("notepad\n");
    await h.waitChange(m, b2, center, 5000, 10000); t.notepad_ms = +(performance.now() - t0).toFixed(1);
    return t;
  },
  // Bake: answer the "Found New Hardware" wizard for v86's VGA controller once, with "don't prompt again".
  async bakePrepare(m, h) {
    const key = (...ks) => { for (const k of ks) m.input.key(k, true); for (const k of ks.reverse()) m.input.key(k, false); };
    key("AltLeft", "KeyT"); await h.sleep(500); key("Enter"); await h.sleep(5000);
    key("Enter"); await h.sleep(30000); await h.shot("bake-wizard-last");
    key("Enter"); await h.sleep(10000);
  },
  async shutdown(m, h) {
    m.input.key("ControlLeft", true); m.input.key("Escape", true); m.input.key("Escape", false); m.input.key("ControlLeft", false);
    await h.sleep(3000); await m.input.type("u"); await h.sleep(4000); await m.input.type("u");
  },
  cursor: { area: { x: 200, y: 100, width: 400, height: 300 }, park: [700, 20], offset: [0, 0] },
};

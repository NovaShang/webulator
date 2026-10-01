// Shared helpers for the classic Mac profiles.
const MENU = { x: 0, y: 20, width: 230, height: 240 };
const CENTER = { x: 160, y: 60, width: 320, height: 240 };

export const menubarReady = img => {                  // a menu bar: black line at y = 19, mostly white above
  for (let x = 0; x < img.width; x += 8) if (img.data[(19 * img.width + x) * 4] !== 0) return false;
  let white = 0; for (let x = 0; x < img.width; x += 8) white += img.data[(3 * img.width + x) * 4] === 255;
  return white > img.width / 16;
};
export const menuTextReady = img => {                 // Mac OS 8/9: light-grey menu bar, dark bottom edge, titles drawn
  const r = (x, y) => img.data[(y * img.width + x) * 4];
  let light = 0, edge = 0, text = 0;
  for (let x = 0; x < img.width; x++) { light += r(x, 2) > 200; edge += r(x, 19) < 100; }
  for (let y = 4; y < 16; y++) for (let x = 40; x < 300; x++) text += r(x, y) < 60;
  return light > img.width * 0.9 && edge > img.width * 0.9 && text > 80;
};

export function cmd(m, code) {
  m.input.key("MetaLeft", true); m.input.key(code, true); m.input.key(code, false); m.input.key("MetaLeft", false);
}

/** Apple menu opens; picking Calculator opens its window. Returns timings in ms. */
export function aliveViaCalculator(calcY) {
  return async (m, h) => {
    const t = {};
    const before = await m.screen.read(MENU);
    m.input.pointer.moveTo(20, 8); await h.sleep(60);
    let t0 = performance.now();
    m.input.pointer.button(0, true);
    await h.waitChange(m, before, MENU, 200, 1000); t.apple_menu_ms = +(performance.now() - t0).toFixed(1);
    m.input.pointer.moveTo(60, calcY); await h.sleep(250);
    const center = await m.screen.read(CENTER);
    t0 = performance.now();
    m.input.pointer.button(0, false);
    await h.waitChange(m, center, CENTER, 1000, 1000); t.calculator_ms = +(performance.now() - t0).toFixed(1);
    return t;
  };
}

/** Special > Shut Down, by dragging through the menu. */
export function shutdownVia(special, item) {
  return async (m, h) => {
    m.input.pointer.moveTo(...special); await h.sleep(60);
    m.input.pointer.button(0, true); await h.sleep(400);
    m.input.pointer.moveTo(...item); await h.sleep(300);
    m.input.pointer.button(0, false);
  };
}

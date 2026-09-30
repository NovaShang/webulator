// Macintosh Plus, System 6.0.8 on Mini vMac (512×342, 1-bit).
import { shutdownVia, menubarReady, aliveViaCalculator, cmd } from "./mac-common.mjs";

// TeachText, opened through the "Welcome!" document, emptied (Select All, Delete) for a blank white page.
async function blankTeachText(m, h) {
  m.input.pointer.moveTo(472, 104); await h.sleep(60);
  for (let i = 0; i < 2; i++) { m.input.pointer.button(0, true); await h.sleep(60); m.input.pointer.button(0, false); await h.sleep(90); }
  await h.sleep(2500); await h.settle(m, 800);
  cmd(m, "KeyA"); await h.sleep(300);
  m.input.key("Backspace", true); m.input.key("Backspace", false);
  await h.sleep(800); await h.settle(m, 800);
}

export default {
  bootTimeout: 60000,
  settle: 2000,
  busyAt: 300,
  shutdown: shutdownVia([185, 8], [200, 123]),
  ready: menubarReady,
  alive: aliveViaCalculator(107),
  cursor: {
    // Over the text area the cursor is the I-beam; its diff box starts 3 px left of and 4 px above the hot spot.
    park: [505, 20], offset: [-3, -4],
    async prepare(m, h) { await blankTeachText(m, h); return { x: 40, y: 60, width: 420, height: 230 }; },
  },
  textEcho: {
    async open(m, h) { await blankTeachText(m, h); return { x: 0, y: 20, width: 512, height: 322 }; },
  },
};

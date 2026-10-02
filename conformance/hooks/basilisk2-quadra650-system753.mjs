// Quadra 650, System 7.5.3 on Basilisk II (640×480).
import { shutdownVia, menubarReady, aliveViaCalculator, cmd, launchFromAppleMenu } from "./mac-common.mjs";

// Bring Stickies to the front by clicking the note on the desktop, File > New Note, then drag its grow box
// so the note covers a large solid area.
async function newStickie(m, h) {
  await h.click(m, 27, 60); await h.sleep(800); await h.settle(m, 600);
  cmd(m, "KeyN"); await h.sleep(1200); await h.settle(m, 800);
  m.input.pointer.moveTo(197, 147); await h.sleep(100);
  m.input.pointer.button(0, true); await h.sleep(150);
  for (let i = 1; i <= 10; i++) { m.input.pointer.moveTo(197 + 36 * i, 147 + 27 * i); await h.sleep(40); }
  m.input.pointer.button(0, false); await h.sleep(800); await h.settle(m, 800);
}

export default {
  bootTimeout: 60000,
  settle: 2000,
  busyAt: 500,
  shutdown: shutdownVia([232, 8], [250, 139]),
  ready: menubarReady,
  alive: aliveViaCalculator(107),
  diskLoad: launchFromAppleMenu(186, 204, 222, 276),   // Jigsaw Puzzle, Key Caps, Note Pad, Scrapbook
  cursor: {
    // Over the note's text the cursor is the I-beam (diff box 3 px left of and 4 px above the hot spot).
    park: [630, 470], offset: [-3, -4],
    async prepare(m, h) { await newStickie(m, h); return { x: 104, y: 100, width: 440, height: 300 }; },
  },
  textEcho: {
    async open(m, h) { await newStickie(m, h); return { x: 0, y: 22, width: 640, height: 440 }; },
  },
};

// Power Macintosh G3, Mac OS 9.0.4 on SheepShaver (640×480).
import { shutdownVia, menuTextReady, aliveViaCalculator, cmd, launchFromAppleMenu } from "./mac-common.mjs";

export default {
  bootTimeout: 120000,
  settle: 3000,
  busyAt: 3000,
  // No shutdown: SheepShaver's core traps when Mac OS 9 powers off (Infinite Mac's build too). T8 boots the
  // exported overlay without a clean shutdown instead.
  // Mac OS 9 then reports the unclean shutdown and runs Disk First Aid; "Done" is the default button, so press
  // Return every few seconds until the Finder is up (harmless while it is still starting).
  async afterUncleanBoot(m, h) {
    const t0 = performance.now();
    while (performance.now() - t0 < 120000) {
      if (menuTextReady(await m.screen.read())) return;
      m.input.key("Enter", true); m.input.key("Enter", false);
      await h.sleep(3000);
    }
  },
  ready: menuTextReady,
  alive: aliveViaCalculator(68),
  diskLoad: launchFromAppleMenu(50, 140, 194, 212),     // Apple System Profiler, Key Caps, Scrapbook, Sherlock 2
  // The desktop is solid; the arrow's white outline starts one pixel up and left of the hot spot.
  cursor: { area: { x: 20, y: 40, width: 400, height: 240 }, park: [300, 8], offset: [-1, -1] },
  textEcho: {
    async open(m, h) {
      // The notes on the desktop are part of the desktop picture; launch Stickies from the Apple menu instead.
      m.input.pointer.moveTo(20, 8); await h.sleep(60);
      m.input.pointer.button(0, true); await h.sleep(400);
      m.input.pointer.moveTo(60, 230); await h.sleep(300);
      m.input.pointer.button(0, false);
      await h.sleep(3000); await h.settle(m, 1000);
      await h.shot?.("T7-step1-stickies");
      cmd(m, "KeyN"); await h.sleep(1200); await h.settle(m, 800);
      await h.shot?.("T7-step2-new");
      return { x: 0, y: 22, width: 640, height: 440 };
    },
  },
};

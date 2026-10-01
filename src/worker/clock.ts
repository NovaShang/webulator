// Virtual clock (spec §4.2). Installed when this module is evaluated, i.e. before any core code is loaded:
// Emscripten's time imports and v86's microtick all end up in Date.now / performance.now.
// Guest time only moves while the machine runs; pauses and the gap between save and restore are invisible.
import type { GuestClock } from "../protocol";

export const realDate = Date.now.bind(Date);
export const realPerf = performance.now.bind(performance);

let offDate = realDate() - realPerf();   // guest date = realPerf() + offDate
let offPerf = 0;                         // guest perf = realPerf() + offPerf
let frozenAt: number | null = null;

const nowReal = () => (frozenAt ?? realPerf());
export const guestDate = () => nowReal() + offDate;
export const guestPerf = () => nowReal() + offPerf;

Date.now = guestDate;
performance.now = guestPerf;

export const clock = {
  start(epochMs: number) { offDate = epochMs - realPerf(); },
  pause() { if (frozenAt === null) frozenAt = realPerf(); },
  resume() {
    if (frozenAt === null) return;
    const gap = realPerf() - frozenAt;
    offDate -= gap; offPerf -= gap; frozenAt = null;
  },
  save(): GuestClock { return { date: guestDate(), perf: guestPerf() }; },
  restore(c: GuestClock) { const r = realPerf(); offDate = c.date - r; offPerf = c.perf - r; frozenAt = null; },
};

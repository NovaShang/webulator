// Binds a machine to a canvas: draws frames (optionally only a viewport of the guest screen) and, with
// `input: true`, forwards pointer, keyboard and wheel events, mapping canvas pixels to guest pixels.
import type { Machine } from "./machine";
import type { AttachOptions } from "./types";

type Frame = { width: number; height: number; rgba: Uint8ClampedArray; seq: number } | null;
const BUTTON_FROM_DOM = [0, 2, 1];   // DOM: 0 left, 1 middle, 2 right → API: 0 primary, 1 secondary, 2 middle

export function attachCanvas(m: Machine, canvas: HTMLCanvasElement, opts: AttachOptions, frame: () => Frame): () => void {
  const ctx = canvas.getContext("2d")!;
  let drawn = -1, raf = 0;
  const view = () => opts.viewport ?? { x: 0, y: 0, width: m.screen.width, height: m.screen.height };

  const draw = () => {
    raf = 0;
    const f = frame();
    if (!f || f.seq === drawn) return;
    drawn = f.seq;
    const v = view();
    if (canvas.width !== v.width || canvas.height !== v.height) { canvas.width = v.width; canvas.height = v.height; }
    ctx.putImageData(new ImageData(f.rgba as Uint8ClampedArray<ArrayBuffer>, f.width, f.height), -v.x, -v.y, v.x, v.y, v.width, v.height);
  };
  const schedule = () => { if (!raf) raf = requestAnimationFrame(draw); };
  const off = [m.on("frame", schedule), m.on("resize", () => { drawn = -1; schedule(); })];
  drawn = -1; schedule();

  const listeners: [EventTarget, string, EventListener][] = [];
  const listen = (t: EventTarget, type: string, fn: (e: any) => void) => {
    t.addEventListener(type, fn, { passive: false } as AddEventListenerOptions); listeners.push([t, type, fn]);
  };

  if (opts.input) {
    const prevCursor = canvas.style.cursor, prevTab = canvas.tabIndex;
    canvas.style.cursor = "none";      // the guest draws its own cursor
    canvas.tabIndex = canvas.tabIndex >= 0 ? canvas.tabIndex : 0;
    const toGuest = (e: PointerEvent | WheelEvent) => {
      const r = canvas.getBoundingClientRect(), v = view();
      return [v.x + ((e.clientX - r.left) / r.width) * v.width, v.y + ((e.clientY - r.top) / r.height) * v.height];
    };
    const buttons = m.info.hardware.pointerButtons;
    listen(canvas, "pointermove", (e: PointerEvent) => { const [x, y] = toGuest(e); m.input.pointer.moveTo(x, y); });
    listen(canvas, "pointerdown", (e: PointerEvent) => {
      canvas.focus(); canvas.setPointerCapture(e.pointerId);
      const [x, y] = toGuest(e); m.input.pointer.moveTo(x, y);
      const b = BUTTON_FROM_DOM[e.button] ?? 0;
      if (b < buttons) m.input.pointer.button(b, true);
      e.preventDefault();
    });
    listen(canvas, "pointerup", (e: PointerEvent) => {
      const b = BUTTON_FROM_DOM[e.button] ?? 0;
      if (b < buttons) m.input.pointer.button(b, false);
      e.preventDefault();
    });
    listen(canvas, "contextmenu", (e: Event) => e.preventDefault());
    if (m.info.hardware.wheel) listen(canvas, "wheel", (e: WheelEvent) => { m.input.pointer.wheel(Math.sign(e.deltaX), Math.sign(e.deltaY)); e.preventDefault(); });
    const key = (down: boolean) => (e: KeyboardEvent) => {
      if (e.repeat && !down) return;
      try { m.input.key(e.code, down); e.preventDefault(); } catch { /* keys the machine does not have */ }
    };
    listen(canvas, "keydown", key(true));
    listen(canvas, "keyup", key(false));
    listeners.push([canvas, "__restore", (() => { canvas.style.cursor = prevCursor; canvas.tabIndex = prevTab; }) as EventListener]);
  }

  return () => {
    off.forEach(f => f());
    if (raf) cancelAnimationFrame(raf);
    for (const [t, type, fn] of listeners) {
      if (type === "__restore") (fn as unknown as () => void)(); else t.removeEventListener(type, fn);
    }
  };
}

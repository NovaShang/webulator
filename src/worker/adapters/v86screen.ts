// A screen adapter for v86 that renders into an RGBA buffer inside the worker (no DOM).
// Graphical modes: copy the VGA layers v86 hands over. Text mode: draw glyphs from the VGA font plane ourselves.
// The text cursor is drawn steadily (no blinking) so an idle screen stops changing.

type Layer = { image_data: ImageData; screen_x: number; screen_y: number; buffer_x: number; buffer_y: number; buffer_width: number; buffer_height: number };
type Vga = { plane2: Uint8Array; max_scan_line: number; screen_fill_buffer(): void; graphical_mode: boolean };

export class HeadlessScreen {
  width = 720; height = 400;
  fb = new Uint8Array(this.width * this.height * 4);
  graphical = false;
  onResize: (w: number, h: number) => void = () => {};
  onDamage: (x: number, y: number, w: number, h: number) => void = () => {};

  private cols = 80; private rows = 25;
  private chars = new Uint8Array(80 * 25);
  private fg = new Int32Array(80 * 25);
  private bg = new Int32Array(80 * 25);
  private changedRows = new Uint8Array(25);
  private fontH = 16; private fontW = 9; private font: Uint8Array | null = null; private copy8th = false;
  private cursorRow = 0; private cursorCol = 0; private curStart = 14; private curEnd = 15; private curOn = true;
  vga: Vga | null = null;

  /** The methods v86's VGA device calls on its screen adapter. */
  methods() {
    return {
      put_char: (row: number, col: number, chr: number, _flags: number, bgc: number, fgc: number) => {
        const i = row * this.cols + col;
        if (i >= this.chars.length) return;
        this.chars[i] = chr; this.bg[i] = bgc; this.fg[i] = fgc; this.changedRows[row] = 1;
      },
      set_mode: (graphical: boolean) => { this.graphical = graphical; if (!graphical) this.changedRows.fill(1); },
      set_size_text: (cols: number, rows: number) => {
        if (cols === this.cols && rows === this.rows && !this.graphical) return;
        this.cols = cols; this.rows = rows;
        this.chars = new Uint8Array(cols * rows); this.fg = new Int32Array(cols * rows); this.bg = new Int32Array(cols * rows);
        this.changedRows = new Uint8Array(rows).fill(1);
        this.resize(cols * this.fontW, rows * this.fontH);
      },
      set_size_graphical: (w: number, h: number) => { this.resize(w, h); },
      set_font_bitmap: (height: number, width9: boolean, widthDbl: boolean, copy8th: boolean, bitmap: Uint8Array) => {
        const w = widthDbl ? 16 : width9 ? 9 : 8;
        this.font = bitmap; this.copy8th = copy8th;
        if (w !== this.fontW || height !== this.fontH) {
          this.fontW = w; this.fontH = height;
          if (!this.graphical) this.resize(this.cols * w, this.rows * height);
        }
        this.changedRows.fill(1);
      },
      set_font_page: () => { this.changedRows.fill(1); },
      clear_screen: () => { this.fb.fill(0); for (let i = 3; i < this.fb.length; i += 4) this.fb[i] = 255; this.onDamage(0, 0, this.width, this.height); },
      clear_text_state: () => {},
      set_scale: () => {},
      update_cursor_scanline: (start: number, end: number, _max: number) => {
        this.curOn = !(start & 0x20) && start <= end; this.curStart = start & 0x1f; this.curEnd = end & 0x1f;
        this.changedRows[this.cursorRow] = 1;
      },
      update_cursor: (row: number, col: number) => {
        this.changedRows[this.cursorRow] = 1; this.cursorRow = row; this.cursorCol = col; this.changedRows[row] = 1;
      },
      update_buffer: (layers: Layer[]) => { for (const l of layers) this.copyLayer(l); },
      destroy: () => {}, pause: () => {}, continue: () => {},
      get_text_screen: () => [], get_text_row: () => "",
    };
  }

  private resize(w: number, h: number) {
    if (w === this.width && h === this.height) return;
    this.width = w; this.height = h;
    this.fb = new Uint8Array(w * h * 4);
    for (let i = 3; i < this.fb.length; i += 4) this.fb[i] = 255;
    this.changedRows.fill(1);
    this.onResize(w, h);
  }

  /**
   * Copy one VGA layer into the frame buffer, clipped on every side. Must never throw: it runs inside v86's
   * screen_fill_buffer, and an exception there skips the vertical-retrace update the guest may be waiting for.
   * v86 re-sends unchanged layers in VGA modes, so only rows that really changed are reported as damage.
   */
  private copyLayer(l: Layer) {
    const src = l.image_data.data, sw = l.image_data.width, sh = l.image_data.height;
    let bx = l.buffer_x, by = l.buffer_y, dx = l.screen_x, dy = l.screen_y, w = l.buffer_width, h = l.buffer_height;
    if (dx < 0) { bx -= dx; w += dx; dx = 0; }
    if (dy < 0) { by -= dy; h += dy; dy = 0; }
    if (bx < 0) { dx -= bx; w += bx; bx = 0; }
    if (by < 0) { dy -= by; h += by; by = 0; }
    w = Math.min(w, this.width - dx, sw - bx);
    h = Math.min(h, this.height - dy, sh - by);
    if (w <= 0 || h <= 0) return;
    let y0 = -1, y1 = -1;
    for (let y = 0; y < h; y++) {
      const s = ((by + y) * sw + bx) * 4, d = ((dy + y) * this.width + dx) * 4, n = w * 4;
      let same = true;
      for (let i = 0; i < n; i += 4) {
        if (src[s + i] !== this.fb[d + i] || src[s + i + 1] !== this.fb[d + i + 1] || src[s + i + 2] !== this.fb[d + i + 2]) { same = false; break; }
      }
      if (same) continue;
      this.fb.set(src.subarray(s, s + n), d);
      if (y0 < 0) y0 = y; y1 = y;
    }
    if (y0 >= 0) this.onDamage(dx, dy + y0, w, y1 - y0 + 1);
  }

  /** Render text-mode rows that changed. */
  renderText() {
    if (this.graphical) return;
    let font = this.font;
    if (!font && this.vga) { font = this.vga.plane2; this.fontH = (this.vga.max_scan_line & 0x1f) + 1 || 16; }
    if (!font) return;
    const fw = this.fontW, fh = this.fontH, W = this.width;
    let y0 = -1, y1 = -1;
    for (let r = 0; r < this.rows; r++) {
      if (!this.changedRows[r]) continue;
      this.changedRows[r] = 0;
      if (y0 < 0) y0 = r; y1 = r;
      for (let c = 0; c < this.cols; c++) {
        const i = r * this.cols + c, ch = this.chars[i], fgc = this.fg[i], bgc = this.bg[i];
        const cursor = this.curOn && r === this.cursorRow && c === this.cursorCol;
        for (let line = 0; line < fh; line++) {
          const bits = font[ch * 32 + line];
          const curLine = cursor && line >= this.curStart && line <= this.curEnd;
          let p = ((r * fh + line) * W + c * fw) * 4;
          for (let b = 0; b < fw; b++, p += 4) {
            let on = b < 8 ? (bits >> (7 - b)) & 1 : (this.copy8th && ch >= 0xc0 && ch <= 0xdf ? bits & 1 : 0);
            if (curLine) on = 1;
            const col = on ? fgc : bgc;
            this.fb[p] = (col >> 16) & 255; this.fb[p + 1] = (col >> 8) & 255; this.fb[p + 2] = col & 255; this.fb[p + 3] = 255;
          }
        }
      }
    }
    if (y0 >= 0) this.onDamage(0, y0 * fh, W, (y1 - y0 + 1) * fh);
  }
}

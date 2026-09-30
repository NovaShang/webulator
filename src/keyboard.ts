// Key identity is the W3C KeyboardEvent.code string (spec §3.5). Cores translate it to their own key codes.

/** Every code the API accepts. The ring carries an index into this list. */
export const KEY_CODES: readonly string[] = [
  "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM",
  "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ",
  "Digit0", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
  "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash", "Semicolon", "Quote", "Backquote",
  "Comma", "Period", "Slash", "Enter", "Tab", "Space", "Backspace", "Escape", "CapsLock",
  "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight",
  "ContextMenu", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown",
  "Insert", "Delete", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
  "F13", "F14", "F15", "NumLock", "ScrollLock", "PrintScreen", "Pause",
  "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
  "NumpadDecimal", "NumpadEnter", "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide", "NumpadEqual",
];
export const KEY_INDEX: ReadonlyMap<string, number> = new Map(KEY_CODES.map((c, i) => [c, i]));

/** Apple Desktop Bus key codes (Apple Extended Keyboard II, as Basilisk II and SheepShaver emulate it). */
export const ADB: Readonly<Record<string, number>> = {
  KeyA: 0x00, KeyS: 0x01, KeyD: 0x02, KeyF: 0x03, KeyH: 0x04, KeyG: 0x05, KeyZ: 0x06, KeyX: 0x07, KeyC: 0x08,
  KeyV: 0x09, KeyB: 0x0b, KeyQ: 0x0c, KeyW: 0x0d, KeyE: 0x0e, KeyR: 0x0f, KeyY: 0x10, KeyT: 0x11,
  Digit1: 0x12, Digit2: 0x13, Digit3: 0x14, Digit4: 0x15, Digit6: 0x16, Digit5: 0x17, Equal: 0x18, Digit9: 0x19,
  Digit7: 0x1a, Minus: 0x1b, Digit8: 0x1c, Digit0: 0x1d, BracketRight: 0x1e, KeyO: 0x1f, KeyU: 0x20,
  BracketLeft: 0x21, KeyI: 0x22, KeyP: 0x23, Enter: 0x24, KeyL: 0x25, KeyJ: 0x26, Quote: 0x27, KeyK: 0x28,
  Semicolon: 0x29, Backslash: 0x2a, Comma: 0x2b, Slash: 0x2c, KeyN: 0x2d, KeyM: 0x2e, Period: 0x2f, Tab: 0x30,
  Space: 0x31, Backquote: 0x32, Backspace: 0x33, Escape: 0x35, MetaLeft: 0x37, MetaRight: 0x37,
  ShiftLeft: 0x38, CapsLock: 0x39, AltLeft: 0x3a, ControlLeft: 0x36, ShiftRight: 0x7b, AltRight: 0x7c,
  ControlRight: 0x7d, ArrowLeft: 0x3b, ArrowRight: 0x3c, ArrowDown: 0x3d, ArrowUp: 0x3e,
  NumpadDecimal: 0x41, NumpadMultiply: 0x43, NumpadAdd: 0x45, NumLock: 0x47, NumpadDivide: 0x4b,
  NumpadEnter: 0x4c, NumpadSubtract: 0x4e, NumpadEqual: 0x51, Numpad0: 0x52, Numpad1: 0x53, Numpad2: 0x54,
  Numpad3: 0x55, Numpad4: 0x56, Numpad5: 0x57, Numpad6: 0x58, Numpad7: 0x59, Numpad8: 0x5b, Numpad9: 0x5c,
  F1: 0x7a, F2: 0x78, F3: 0x63, F4: 0x76, F5: 0x60, F6: 0x61, F7: 0x62, F8: 0x64, F9: 0x65, F10: 0x6d,
  F11: 0x67, F12: 0x6f, F13: 0x69, F14: 0x6b, F15: 0x71, Home: 0x73, PageUp: 0x74, Delete: 0x75, End: 0x77,
  PageDown: 0x79, Insert: 0x72,
};

/** Mini vMac's key codes differ from ADB for a few keys (it models the Mac Plus keyboard). */
export const MINIVMAC: Readonly<Record<string, number>> = {
  ...ADB,
  ArrowLeft: 0x7b, ArrowRight: 0x7c, ArrowDown: 0x7d, ArrowUp: 0x7e,
  ShiftRight: 0x38, AltRight: 0x3a, ControlLeft: 0x3b, ControlRight: 0x3b,
};

/** PC/AT scan code set 1 make codes; 0xE0xx marks an extended key. Break code = make | 0x80. */
export const PS2_SET1: Readonly<Record<string, number>> = {
  Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06, Digit6: 0x07, Digit7: 0x08,
  Digit8: 0x09, Digit9: 0x0a, Digit0: 0x0b, Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18,
  KeyP: 0x19, BracketLeft: 0x1a, BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d, KeyA: 0x1e, KeyS: 0x1f,
  KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27,
  Quote: 0x28, Backquote: 0x29, ShiftLeft: 0x2a, Backslash: 0x2b, KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e,
  KeyV: 0x2f, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32, Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
  NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3a, F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e,
  F5: 0x3f, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44, NumLock: 0x45, ScrollLock: 0x46,
  Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a, Numpad4: 0x4b, Numpad5: 0x4c,
  Numpad6: 0x4d, NumpadAdd: 0x4e, Numpad1: 0x4f, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52,
  NumpadDecimal: 0x53, F11: 0x57, F12: 0x58,
  NumpadEnter: 0xe01c, ControlRight: 0xe01d, NumpadDivide: 0xe035, AltRight: 0xe038, Home: 0xe047,
  ArrowUp: 0xe048, PageUp: 0xe049, ArrowLeft: 0xe04b, ArrowRight: 0xe04d, End: 0xe04f, ArrowDown: 0xe050,
  PageDown: 0xe051, Insert: 0xe052, Delete: 0xe053, MetaLeft: 0xe05b, MetaRight: 0xe05c, ContextMenu: 0xe05d,
};

/** US layout: character → [code, needs shift]. */
export function charToKey(ch: string): [string, boolean] | null {
  const plain = "abcdefghijklmnopqrstuvwxyz";
  const i = plain.indexOf(ch);
  if (i >= 0) return ["Key" + ch.toUpperCase(), false];
  const j = plain.toUpperCase().indexOf(ch);
  if (j >= 0) return ["Key" + ch, true];
  const digits = "0123456789", shifted = ")!@#$%^&*(";
  if (digits.includes(ch)) return ["Digit" + ch, false];
  if (shifted.includes(ch)) return ["Digit" + shifted.indexOf(ch), true];
  const map: Record<string, [string, boolean]> = {
    " ": ["Space", false], "\n": ["Enter", false], "\t": ["Tab", false],
    "-": ["Minus", false], "_": ["Minus", true], "=": ["Equal", false], "+": ["Equal", true],
    "[": ["BracketLeft", false], "{": ["BracketLeft", true], "]": ["BracketRight", false], "}": ["BracketRight", true],
    "\\": ["Backslash", false], "|": ["Backslash", true], ";": ["Semicolon", false], ":": ["Semicolon", true],
    "'": ["Quote", false], "\"": ["Quote", true], "`": ["Backquote", false], "~": ["Backquote", true],
    ",": ["Comma", false], "<": ["Comma", true], ".": ["Period", false], ">": ["Period", true],
    "/": ["Slash", false], "?": ["Slash", true],
  };
  return map[ch] ?? null;
}

// Injected with --pre-js: on a cold boot main() starts inside run() and never returns, so this must be in
// place before run(). The closures resolve Asyncify / wasmExports lazily, after they exist.
Module["snap"] = {
  get asyncify() { return typeof Asyncify !== "undefined" ? Asyncify : null; },
  exports: () => wasmExports,
  stackSave: () => _emscripten_stack_get_current(),
  stackRestore: (sp) => __emscripten_stack_restore(sp),
  startRewind: (p) => _asyncify_start_rewind(p),
};

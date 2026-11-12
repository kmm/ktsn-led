// Types for the Emscripten module built by ide/wasm/build.sh (generated/ktsn-led.mjs).
declare module '*/generated/ktsn-led.mjs' {
  export interface KtsnLedModule {
    cwrap(name: string, ret: 'number' | 'string' | null, args: ('number' | 'string')[]): (...a: (number | string)[]) => number;
    UTF8ToString(ptr: number): string;
    HEAPU32: Uint32Array;
    HEAPU8: Uint8Array;
  }
  const createModule: () => Promise<KtsnLedModule>;
  export default createModule;
}

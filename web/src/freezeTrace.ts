/** DESIGN.md, Cortex-M trace: explicit byte format keeps split messages bounded. */
export interface FreezeTrace {
  state: number;
  flags: number;
  waitQueue: number;
  stackPointer: number;
  addresses: number[];
}

export function decodeFreezeTrace(bytes: Uint8Array): FreezeTrace | null {
  if (
    bytes.length !== 36 ||
    bytes[0] !== 1 ||
    bytes[3] > 6 ||
    (bytes[2] & ~31) !== 0
  ) {
    return null;
  }
  if (bytes[3] !== 0 && (bytes[2] & 1) === 0) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const addresses = Array.from({ length: bytes[3] }, (_, i) =>
    view.getUint32(12 + 4 * i, true)
  );
  return {
    state: bytes[1],
    flags: bytes[2],
    waitQueue: view.getUint32(4, true),
    stackPointer: view.getUint32(8, true),
    addresses,
  };
}

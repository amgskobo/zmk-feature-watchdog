/** Common capture v1. Unknown bytes remain in copied reports. */
export function decodeCaptureContext(bytes: Uint8Array) {
  if (
    bytes.length !== 192 ||
    bytes[0] !== 1 ||
    bytes[1] & ~127 ||
    bytes[2] > 6 ||
    bytes[3] > 4
  )
    return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const word = (offset: number) => view.getUint32(offset, true);
  if (word(92) & ~1) return null;
  return {
    flags: bytes[1],
    timeMs: word(92) & 1 ? word(4) : null,
    assertionSite: word(8),
    assertionLine: word(12),
    stackPointer: word(16),
    mmfar: word(20),
    bfar: word(24),
    icsr: word(28),
    callee:
      bytes[1] & 2
        ? Array.from({ length: 8 }, (_, i) => word(32 + i * 4))
        : null,
    addresses: Array.from({ length: bytes[2] }, (_, i) => word(64 + i * 4)),
    dropped: word(88),
    operations: Array.from({ length: bytes[3] }, (_, i) => {
      const at = 96 + i * 24;
      return {
        sequence: word(at),
        timeMs: word(at + 4),
        site: word(at + 8),
        domain: view.getUint16(at + 12, true),
        event: view.getUint16(at + 14, true),
        arg0: word(at + 16),
        arg1: word(at + 20),
      };
    }),
  };
}

/* Fixed format; malformed/unknown bytes stay available in the copied report. */
export const eventNames: Record<number, string> = {
  1: "Boot",
  2: "Active profile",
  3: "Endpoint",
  4: "USB state",
  10: "BT_CLR begin",
  11: "BT_CLR returned",
  12: "Unpair begin",
  13: "Unpair returned",
  14: "Disconnect requested (handle)",
  15: "Disconnect returned",
  16: "Connected (handle / error)",
  17: "Disconnected (reason)",
  18: "Security (error / level)",
  19: "Disconnect request reason",
  20: "Monitor arm result (queue index)",
  21: "Pending conversion",
  22: "Reset audit error",
};
export function decodeDiagnostics(bytes: Uint8Array) {
  if (
    bytes.length !== 80 ||
    bytes[0] !== 1 ||
    bytes[1] > 7 ||
    bytes[2] & ~7 ||
    bytes[3] & ~1
  )
    return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return {
    flags: bytes[2],
    observedBle: !!bytes[3],
    buildTag: view.getUint32(4, true).toString(16).padStart(8, "0"),
    profile: bytes[8],
    endpoint: bytes[9],
    usb: bytes[10],
    links: bytes[11],
    dropped: view.getUint32(12, true),
    cfsr: view.getUint32(16, true),
    hfsr: view.getUint32(20, true),
    events: Array.from({ length: bytes[1] }, (_, index) => {
      const offset = 24 + index * 8,
        event = bytes[offset + 4],
        value = view.getUint16(offset + 6, true);
      return {
        timeMs: view.getUint32(offset, true),
        event,
        profile: bytes[offset + 5],
        value,
        result:
          [13, 15, 20, 21, 22].includes(event) && value >= 0x8000
            ? value - 0x10000
            : value,
      };
    }),
  };
}
export function decodeFatalContext(bytes: Uint8Array) {
  if (bytes.length !== 24) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return Array.from({ length: 6 }, (_, index) =>
    view.getUint32(index * 4, true)
  );
}

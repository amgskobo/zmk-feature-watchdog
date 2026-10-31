import {
  decodeDiagnostics,
  decodeFatalContext,
} from "../../web/src/diagnostics";
test("diagnostics preserve capture order across uptime wrap and signed return values", () => {
  const bytes = new Uint8Array(80);
  const v = new DataView(bytes.buffer);
  bytes.set([1, 2, 3, 1]);
  v.setUint32(4, 0x12345678, true);
  bytes.set([3, 2, 0, 1], 8);
  v.setUint32(12, 1, true);
  v.setUint32(24, 0xfffffff0, true);
  bytes[28] = 10;
  bytes[29] = 3;
  v.setUint32(32, 1, true);
  bytes[36] = 15;
  bytes[37] = 3;
  v.setUint16(38, 65514, true);
  const diagnostic = decodeDiagnostics(bytes)!;
  expect(diagnostic.buildTag).toBe("12345678");
  expect(diagnostic.profile).toBe(3);
  expect(diagnostic.usb).toBe(0);
  expect(diagnostic.events.map((e) => [e.timeMs, e.event, e.result])).toEqual([
    [0xfffffff0, 10, 0],
    [1, 15, -22],
  ]);
  expect(decodeDiagnostics(bytes.subarray(1))).toBeNull();
  bytes[1] = 8;
  expect(decodeDiagnostics(bytes)).toBeNull();
  bytes[1] = 2;
  bytes[2] = 8;
  expect(decodeDiagnostics(bytes)).toBeNull();
});
test("fatal context uses exactly six unsigned little-endian words", () => {
  const bytes = new Uint8Array(24),
    v = new DataView(bytes.buffer);
  [3, 0, 0xffffffff, 4, 12, 0x1000011].forEach((value, i) =>
    v.setUint32(i * 4, value, true),
  );
  expect(decodeFatalContext(bytes)).toEqual([
    3, 0, 0xffffffff, 4, 12, 0x1000011,
  ]);
  expect(decodeFatalContext(new Uint8Array())).toBeNull();
});

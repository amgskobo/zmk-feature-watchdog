import { decodeCaptureContext } from "../../web/src/captureContext";

test("common capture preserves unsigned metadata and capture order", () => {
  const bytes = new Uint8Array(192),
    v = new DataView(bytes.buffer);
  bytes.set([1, 2 | 64, 2, 2]);
  for (let i = 0; i < 8; i++) v.setUint32(32 + i * 4, i + 4, true);
  v.setUint32(64, 0x1000, true);
  v.setUint32(68, 0x2001, true);
  [0xfffffff0, 1].forEach((time, i) => {
    const at = 96 + i * 24;
    v.setUint32(at, i + 1, true);
    v.setUint32(at + 4, time, true);
    v.setUint16(at + 12, 0xffff, true);
    v.setUint16(at + 14, i, true);
    v.setUint32(at + 16, 0xffffffff, true);
    v.setUint32(at + 20, 0x80000000, true);
  });
  const context = decodeCaptureContext(bytes)!;
  expect(context.timeMs).toBeNull();
  v.setUint32(92, 1, true);
  expect(decodeCaptureContext(bytes)!.timeMs).toBe(0);
  expect(context.callee).toEqual([4, 5, 6, 7, 8, 9, 10, 11]);
  expect(context.operations.map((x) => x.timeMs)).toEqual([0xfffffff0, 1]);
  expect(context.operations[0].arg1).toBe(0x80000000);
  for (const [at, value] of [
    [0, 2],
    [1, 128],
    [2, 7],
    [3, 5],
  ]) {
    const malformed = bytes.slice();
    malformed[at] = value;
    expect(decodeCaptureContext(malformed)).toBeNull();
  }
  expect(decodeCaptureContext(new Uint8Array())).toBeNull();
  v.setUint32(92, 2, true);
  expect(decodeCaptureContext(bytes)).toBeNull();
});

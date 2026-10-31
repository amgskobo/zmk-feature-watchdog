import { decodeFreezeTrace } from "../../web/src/freezeTrace";
import { Incident } from "../../web/src/proto/cormoran/watchdog/watchdog";

// Independent wire bytes, including unsigned addresses and a nonzero view offset.
const wire = Uint8Array.from([
  1,
  2,
  5,
  2,
  0x24,
  1,
  0,
  0x20,
  0,
  0x10,
  0,
  0x20,
  0x20,
  0x10,
  0,
  0,
  0x31,
  0x20,
  0,
  0x80,
  ...Array(16).fill(0),
]);

it("decodes trace metadata and addresses in wire order", () => {
  const wrapped = new Uint8Array(40);
  wrapped.set(wire, 2);
  expect(decodeFreezeTrace(wrapped.subarray(2, 38))).toEqual({
    state: 2,
    flags: 5,
    waitQueue: 0x20000124,
    stackPointer: 0x20001000,
    addresses: [0x1020, 0x80002031],
  });
});

it("rejects absent, truncated, unknown and inconsistent trace formats", () => {
  for (let length = 0; length < 36; length++) {
    expect(decodeFreezeTrace(wire.slice(0, length))).toBeNull();
  }
  for (const [offset, value] of [
    [0, 2],
    [2, 32],
    [2, 4],
    [3, 7],
  ]) {
    const bad = wire.slice();
    bad[offset] = value;
    expect(decodeFreezeTrace(bad)).toBeNull();
  }
  expect(decodeFreezeTrace(new Uint8Array(37))).toBeNull();
});

it("retains optional traces through the real generated protobuf codec", () => {
  const current = Incident.create({
    freeze: { channelId: 2, queueName: "sysworkq", trace: wire },
  });
  expect(
    Incident.decode(Incident.encode(current).finish()).freeze?.trace,
  ).toEqual(wire);
  const oldWire = Uint8Array.from([
    0x32,
    0x0a,
    0x12,
    8,
    ...new TextEncoder().encode("sysworkq"),
  ]);
  expect(Incident.decode(oldWire).freeze?.trace).toEqual(new Uint8Array());
});

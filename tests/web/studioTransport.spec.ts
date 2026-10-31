import { ReadableStream, WritableStream } from "node:stream/web";
import { connectSerial, connectBluetooth } from "../../web/src/studioTransport";

beforeAll(() => Object.assign(globalThis, { ReadableStream, WritableStream }));
afterEach(() => {
  Object.defineProperty(navigator, "serial", {
    configurable: true,
    value: undefined,
  });
  Object.defineProperty(navigator, "bluetooth", {
    configurable: true,
    value: undefined,
  });
});
function serialPort() {
  return {
    readable: new ReadableStream<Uint8Array>(),
    writable: new WritableStream<Uint8Array>(),
    open: jest.fn().mockResolvedValue(undefined),
    setSignals: jest.fn().mockResolvedValue(undefined),
    close: jest.fn().mockResolvedValue(undefined),
  };
}
it("USB cleanup waits for RPC locks, closes exactly once, and enables DTR", async () => {
  const port = serialPort();
  Object.defineProperty(navigator, "serial", {
    configurable: true,
    value: { requestPort: async () => port },
  });
  const transport = await connectSerial();
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  const closing = transport.close();
  await Promise.resolve();
  expect(port.close).not.toHaveBeenCalled();
  reader.releaseLock();
  writer.releaseLock();
  await closing;
  await transport.close();
  expect(port.close).toHaveBeenCalledTimes(1);
  expect(port.setSignals).toHaveBeenCalledWith({ dataTerminalReady: true });
});
it("USB setup failure releases an already acquired port", async () => {
  const port = serialPort();
  port.setSignals.mockRejectedValue(new Error("signal setup failed"));
  Object.defineProperty(navigator, "serial", {
    configurable: true,
    value: { requestPort: async () => port },
  });
  await expect(connectSerial()).rejects.toThrow("signal setup failed");
  expect(port.close).toHaveBeenCalledTimes(1);
});
it("unsupported USB and Bluetooth do not try another transport or auto-connect", async () => {
  await expect(connectSerial()).rejects.toThrow("unavailable");
  await expect(connectBluetooth(jest.fn())).rejects.toThrow("unavailable");
});
function bluetooth() {
  const characteristic = new EventTarget() as EventTarget & {
    value?: DataView;
    properties: { write: boolean };
    startNotifications: jest.Mock;
    stopNotifications: jest.Mock;
    writeValueWithResponse: jest.Mock;
    writeValueWithoutResponse: jest.Mock;
  };
  Object.assign(characteristic, {
    properties: { write: true },
    startNotifications: jest.fn().mockResolvedValue(undefined),
    stopNotifications: jest.fn().mockResolvedValue(undefined),
    writeValueWithResponse: jest.fn().mockResolvedValue(undefined),
    writeValueWithoutResponse: jest.fn().mockResolvedValue(undefined),
  });
  const device = new EventTarget() as EventTarget & {
    gatt: {
      connected: boolean;
      connect: jest.Mock;
      disconnect: jest.Mock;
      getPrimaryService: jest.Mock;
    };
  };
  device.gatt = {
    connected: true,
    connect: jest.fn().mockResolvedValue(undefined),
    disconnect: jest.fn(),
    getPrimaryService: jest
      .fn()
      .mockResolvedValue({ getCharacteristic: async () => characteristic }),
  };
  const requestDevice = jest.fn().mockResolvedValue(device);
  Object.defineProperty(navigator, "bluetooth", {
    configurable: true,
    value: { requestDevice },
  });
  return { device, characteristic, requestDevice };
}
it("Bluetooth subscribes before a first notification, preserves DataView bounds and releases resources", async () => {
  const { characteristic, device, requestDevice } = bluetooth();
  characteristic.startNotifications.mockImplementation(async () => {
    characteristic.value = new DataView(
      new Uint8Array([99, 11, 22, 88]).buffer,
      1,
      2,
    );
    characteristic.dispatchEvent(new Event("characteristicvaluechanged"));
  });
  const lost = jest.fn();
  const transport = await connectBluetooth(lost);
  const reader = transport.readable.getReader();
  expect(Array.from((await reader.read()).value!)).toEqual([11, 22]);
  reader.releaseLock();
  const writer = transport.writable.getWriter();
  await writer.write(new Uint8Array([1, 2]));
  writer.releaseLock();
  expect(characteristic.writeValueWithResponse).toHaveBeenCalledTimes(1);
  expect(characteristic.writeValueWithoutResponse).not.toHaveBeenCalled();
  expect(requestDevice).toHaveBeenCalledTimes(1);
  expect(requestDevice).toHaveBeenCalledWith({
    acceptAllDevices: true,
    optionalServices: ["00000000-0196-6107-c967-c5cfb1c2482a"],
  });
  await transport.close();
  await transport.close();
  expect(device.gatt.disconnect).toHaveBeenCalledTimes(1);
  device.dispatchEvent(new Event("gattserverdisconnected"));
  expect(lost).not.toHaveBeenCalled();
});
it("Bluetooth partial setup failure disconnects without leaving a service owned", async () => {
  const { device } = bluetooth();
  device.gatt.getPrimaryService.mockRejectedValue(new Error("service missing"));
  await expect(connectBluetooth(jest.fn())).rejects.toThrow("service missing");
  expect(device.gatt.disconnect).toHaveBeenCalledTimes(1);
});
it("Bluetooth peer loss notifies the owner and prevents later notifications", async () => {
  const { device, characteristic } = bluetooth();
  const lost = jest.fn();
  const transport = await connectBluetooth(lost);
  device.gatt.connected = false;
  device.dispatchEvent(new Event("gattserverdisconnected"));
  await transport.close();
  expect(lost).toHaveBeenCalledTimes(1);
  characteristic.value = new DataView(new Uint8Array([1]).buffer);
  characteristic.dispatchEvent(new Event("characteristicvaluechanged"));
  expect((await transport.readable.getReader().read()).done).toBe(true);
});

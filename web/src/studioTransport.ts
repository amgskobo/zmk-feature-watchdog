import type { RpcTransport } from "@zmkfirmware/zmk-studio-ts-client/transport/index";

export interface ManagedTransport extends RpcTransport {
  close(): Promise<void>;
}

// Only the public browser APIs used by these transports are required.
interface SerialPort {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number }): Promise<void>;
  setSignals(signals: { dataTerminalReady: boolean }): Promise<void>;
  close(): Promise<void>;
}
interface Characteristic extends EventTarget {
  value?: DataView;
  properties: { write: boolean };
  startNotifications(): Promise<unknown>;
  stopNotifications(): Promise<unknown>;
  writeValueWithResponse(value: Uint8Array): Promise<void>;
  writeValueWithoutResponse(value: Uint8Array): Promise<void>;
}
interface BluetoothDevice extends EventTarget {
  gatt?: {
    connected: boolean;
    connect(): Promise<unknown>;
    disconnect(): void;
    getPrimaryService(uuid: string): Promise<{
      getCharacteristic(uuid: string): Promise<Characteristic>;
    }>;
  };
}
type BrowserNavigator = Navigator & {
  serial?: { requestPort(): Promise<SerialPort> };
  bluetooth?: {
    requestDevice(options: {
      acceptAllDevices: true;
      optionalServices: string[];
    }): Promise<BluetoothDevice>;
  };
};
const SERVICE = "00000000-0196-6107-c967-c5cfb1c2482a";
const CHANNEL = "00000001-0196-6107-c967-c5cfb1c2482a";

export async function connectSerial(): Promise<ManagedTransport> {
  const api = (navigator as BrowserNavigator).serial;
  if (!api) throw new Error("USB connection is unavailable in this browser.");
  const port = await api.requestPort();
  try {
    await port.open({ baudRate: 115200 });
    await port.setSignals({ dataTerminalReady: true });
    if (!port.readable || !port.writable)
      throw new Error("USB streams unavailable.");
  } catch (error) {
    await port.close().catch(() => {});
    throw error;
  }
  const abortController = new AbortController();
  let closing: Promise<void> | undefined;
  const close = () => {
    closing ??= Promise.resolve().then(async () => {
      abortController.abort();
      // The RPC pipeline must be aborted by its owner before releasing a port.
      // Never call cancel()/close() on streams still locked by that pipeline.
      const deadline = Date.now() + 1000;
      while (port.readable?.locked || port.writable?.locked) {
        if (Date.now() >= deadline) {
          throw new Error(
            "USB cleanup could not finish. Close this tab to release the port."
          );
        }
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
      await port.readable?.cancel().catch(() => {});
      await port.writable?.abort().catch(() => {});
      await port.close();
    });
    return closing;
  };
  abortController.signal.addEventListener(
    "abort",
    () => {
      void close().catch(() => {});
    },
    { once: true }
  );
  return {
    label: "USB Studio",
    abortController,
    readable: port.readable!,
    writable: port.writable!,
    close,
  };
}

export async function connectBluetooth(
  onDisconnect: () => void
): Promise<ManagedTransport> {
  const api = (navigator as BrowserNavigator).bluetooth;
  if (!api)
    throw new Error("Bluetooth connection is unavailable in this browser.");
  const device = await api.requestDevice({
    acceptAllDevices: true,
    optionalServices: [SERVICE],
  });
  if (!device.gatt) throw new Error("Bluetooth service unavailable.");
  const gatt = device.gatt;
  const abortController = new AbortController();
  let characteristic: Characteristic | undefined;
  let controller: ReadableStreamDefaultController<Uint8Array>;
  let closing: Promise<void> | undefined;
  let closed = false;
  const readable = new ReadableStream<Uint8Array>({
    start(value) {
      controller = value;
    },
  });
  const received = () => {
    const value = characteristic?.value;
    if (!closed && value) {
      controller.enqueue(
        new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice()
      );
    }
  };
  const disconnected = () => {
    if (closed) return;
    onDisconnect();
    void close().catch(() => {});
  };
  const close = () => {
    closing ??= Promise.resolve().then(async () => {
      closed = true;
      abortController.abort();
      characteristic?.removeEventListener(
        "characteristicvaluechanged",
        received
      );
      device.removeEventListener("gattserverdisconnected", disconnected);
      try {
        if (gatt.connected) await characteristic?.stopNotifications();
      } finally {
        gatt.disconnect();
        try {
          controller.close();
        } catch {
          /* The RPC pipeline may already have cancelled it. */
        }
      }
    });
    return closing;
  };
  try {
    await gatt.connect();
    const service = await gatt.getPrimaryService(SERVICE);
    characteristic = await service.getCharacteristic(CHANNEL);
    await characteristic.stopNotifications();
    characteristic.addEventListener("characteristicvaluechanged", received);
    device.addEventListener("gattserverdisconnected", disconnected);
    await characteristic.startNotifications();
  } catch (error) {
    await close().catch(() => {});
    throw error;
  }
  const channel = characteristic;
  const writable = new WritableStream<Uint8Array>({
    write(value) {
      return channel.properties.write
        ? channel.writeValueWithResponse(value)
        : channel.writeValueWithoutResponse(value);
    },
  });
  abortController.signal.addEventListener(
    "abort",
    () => {
      void close().catch(() => {});
    },
    { once: true }
  );
  return {
    label: "Bluetooth Studio",
    abortController,
    readable,
    writable,
    close,
  };
}

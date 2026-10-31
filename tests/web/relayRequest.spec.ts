import { callWithRelay } from "../../web/src/relayRequest";
import {
  Notification,
  Response,
} from "../../web/src/proto/cormoran/watchdog/watchdog";

it("retains an answer arriving before deferred ack and matches source", async () => {
  let callback!: (value: Notification) => void;
  const unsubscribe = jest.fn();
  const result = await callWithRelay(
    async () => {
      callback(
        Notification.create({
          peripheralResponse: {
            source: 2,
            requestId: 7,
            response: { status: { capacity: 99 } },
          },
        }),
      );
      callback(
        Notification.create({
          peripheralResponse: {
            source: 1,
            requestId: 7,
            response: { status: { capacity: 16 } },
          },
        }),
      );
      return Response.create({ deferred: { requestId: 7 } });
    },
    (fn) => {
      callback = fn;
      return unsubscribe;
    },
    1,
    100,
  );
  expect(result?.status?.capacity).toBe(16);
  expect(unsubscribe).toHaveBeenCalledTimes(1);
});

it("times out the whole request and unsubscribes a missing peripheral", async () => {
  jest.useFakeTimers();
  const unsubscribe = jest.fn();
  try {
    const pending = callWithRelay(
      () => new Promise(() => {}),
      () => unsubscribe,
      1,
      100,
    );
    await jest.advanceTimersByTimeAsync(100);
    expect(await pending).toBeNull();
    expect(unsubscribe).toHaveBeenCalledTimes(1);
  } finally {
    jest.useRealTimers();
  }
});

it("aborts and releases subscription without waiting for the device", async () => {
  const controller = new AbortController();
  const unsubscribe = jest.fn();
  const pending = callWithRelay(
    () => new Promise(() => {}),
    () => unsubscribe,
    1,
    1000,
    controller.signal,
  );
  controller.abort();
  expect(await pending).toBeNull();
  expect(unsubscribe).toHaveBeenCalledTimes(1);
});

it("gives the RPC and acknowledged relay separate bounded budgets", async () => {
  jest.useFakeTimers();
  let ack!: (value: Response) => void;
  let callback!: (value: Notification) => void;
  const unsubscribe = jest.fn();
  try {
    const pending = callWithRelay(
      () =>
        new Promise((resolve) => {
          ack = resolve;
        }),
      (fn) => {
        callback = fn;
        return unsubscribe;
      },
      1,
      100,
      undefined,
      1000,
    );
    await jest.advanceTimersByTimeAsync(500);
    ack(Response.create({ deferred: { requestId: 9 } }));
    await jest.advanceTimersByTimeAsync(99);
    callback(
      Notification.create({
        peripheralResponse: {
          source: 1,
          requestId: 9,
          response: { status: { capacity: 16 } },
        },
      }),
    );
    expect((await pending)?.status?.capacity).toBe(16);
    expect(unsubscribe).toHaveBeenCalledTimes(1);
  } finally {
    jest.useRealTimers();
  }
});

it("cleans up a rejected send and a failed subscription", async () => {
  jest.useFakeTimers();
  const unsubscribe = jest.fn();
  try {
    await expect(
      callWithRelay(
        async () => {
          throw new Error("transport failed");
        },
        () => unsubscribe,
        1,
        100,
      ),
    ).rejects.toThrow("transport failed");
    expect(unsubscribe).toHaveBeenCalledTimes(1);
    await expect(
      callWithRelay(
        async () => Response.create(),
        () => {
          throw new Error("subscription failed");
        },
        1,
        100,
      ),
    ).rejects.toThrow("subscription failed");
    expect(jest.getTimerCount()).toBe(0);
  } finally {
    jest.useRealTimers();
  }
});

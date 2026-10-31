import { Notification, Response } from "./proto/cormoran/watchdog/watchdog";

// Subscribe before sending: a fast peripheral can answer before the RPC ack.
// Unrelated broadcasts are bounded, and are never attributed by request ID alone.
export async function callWithRelay(
  send: () => Promise<Response | null>,
  subscribe: (callback: (value: Notification) => void) => () => void,
  source: number,
  timeoutMs: number,
  signal?: AbortSignal,
  rpcTimeoutMs = timeoutMs
): Promise<Response | null> {
  const early = new Map<number, Response | null>();
  let expected: number | undefined;
  let finish!: (value: Response | null) => void;
  const answer = new Promise<Response | null>((resolve) => {
    finish = resolve;
  });
  const abort = () => finish(null);
  let timer = setTimeout(abort, rpcTimeoutMs);
  let unsubscribe = () => {};
  try {
    unsubscribe = subscribe((notification) => {
      const value = notification.peripheralResponse;
      if (!value || value.source !== source) return;
      if (expected !== undefined) {
        if (value.requestId === expected) finish(value.response ?? null);
      } else if (!early.has(value.requestId) && early.size < 8) {
        early.set(value.requestId, value.response ?? null);
      }
    });
    signal?.addEventListener("abort", abort, { once: true });
    if (signal?.aborted) return null;
    const response = await Promise.race([send(), answer]);
    if (!response?.deferred) return response;
    expected = response.deferred.requestId;
    if (early.has(expected)) return early.get(expected) ?? null;
    clearTimeout(timer);
    timer = setTimeout(abort, timeoutMs);
    return await answer;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener("abort", abort);
    unsubscribe();
  }
}

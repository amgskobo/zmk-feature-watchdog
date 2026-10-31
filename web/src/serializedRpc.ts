// Serialize all dashboard/status/history calls on this connection. An aborted
// queued request is skipped; a failed request never poisons the queue.
export function serializeRpc<Q, R>(
  call: (request: Q, signal?: AbortSignal) => Promise<R | null>
) {
  let tail: Promise<unknown> = Promise.resolve();
  return (request: Q, signal?: AbortSignal): Promise<R | null> => {
    const next = tail.then(() =>
      signal?.aborted ? null : call(request, signal)
    );
    tail = next.then(
      () => undefined,
      () => undefined
    );
    return next;
  };
}

// Console logs accept only fixed error categories; UI messages remain separate.
const ERROR_KINDS = new Set([
  "Error",
  "TypeError",
  "RangeError",
  "SyntaxError",
  "AbortError",
  "NotAllowedError",
  "NotFoundError",
  "NetworkError",
  "SecurityError",
  "TimeoutError",
  "InvalidStateError",
  "NotSupportedError",
  "OperationError",
  "DataError",
  "UnknownError",
]);

export function errorKind(error: unknown): string {
  try {
    if (typeof error !== "object" || error === null) return "UnknownError";
    const name = (error as { name?: unknown }).name;
    return typeof name === "string" && ERROR_KINDS.has(name)
      ? name
      : "UnknownError";
  } catch {
    return "UnknownError";
  }
}

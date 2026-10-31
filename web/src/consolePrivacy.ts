import { errorKind } from "./logPrivacy";

// Console output carries only fixed labels, numbers, booleans and error
// categories, from this app and from the Studio SDK packages alike. It is
// applied at runtime to the console itself, so neither is rewritten.
//
// The first argument is kept when it is a string: by convention a fixed label.
// tests/web/sdkConsole.spec.ts holds the pinned SDK to that convention. Any
// other string becomes "[redacted]", an error or other object its error
// category, and a plain object keeps its keys with each value redacted.
const METHODS = ["debug", "log", "info", "warn", "error"] as const;
const MAX_DEPTH = 2;

function isPlainObject(value: object): boolean {
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function redactValue(value: unknown, depth: number): unknown {
  if (
    value === null ||
    value === undefined ||
    typeof value === "number" ||
    typeof value === "boolean"
  ) {
    return value;
  }
  if (typeof value === "string") {
    return "[redacted]";
  }
  try {
    if (
      typeof value === "object" &&
      depth < MAX_DEPTH &&
      isPlainObject(value)
    ) {
      const redacted: Record<string, unknown> = {};
      for (const key of Object.keys(value)) {
        redacted[key] = redactValue(
          (value as Record<string, unknown>)[key],
          depth + 1
        );
      }
      return redacted;
    }
  } catch {
    return "UnknownError";
  }
  return errorKind(value);
}

export function redactConsoleArgs(args: readonly unknown[]): unknown[] {
  return args.map((arg, index) =>
    index === 0 && typeof arg === "string" ? arg : redactValue(arg, 0)
  );
}

export function installConsolePrivacy(target: Console = console): void {
  for (const method of METHODS) {
    const original = target[method];
    if (typeof original !== "function") {
      continue;
    }
    target[method] = (...args: unknown[]) => {
      original.apply(target, redactConsoleArgs(args));
    };
  }
}

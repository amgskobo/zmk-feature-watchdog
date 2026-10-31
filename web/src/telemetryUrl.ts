declare const __WATCHDOG_TELEMETRY_URL__: string;

const configuredUrl =
  typeof __WATCHDOG_TELEMETRY_URL__ === "string"
    ? __WATCHDOG_TELEMETRY_URL__
    : "";

export function getTelemetryUrl(value: unknown = configuredUrl): string | null {
  if (typeof value !== "string" || !value.trim()) return null;
  try {
    const url = new URL(value.trim());
    if (
      (url.protocol !== "http:" && url.protocol !== "https:") ||
      url.username ||
      url.password
    ) {
      return null;
    }
    return url.href;
  } catch {
    return null;
  }
}

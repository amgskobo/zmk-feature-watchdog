import { getTelemetryUrl } from "../../web/src/telemetryUrl";

it("has no default telemetry endpoint", () => {
  expect(getTelemetryUrl()).toBeNull();
});

it.each([null, 0, {}, "", "  ", "/console", "not a URL"])(
  "rejects absent, non-string or relative configuration: %p",
  (value) => {
    expect(getTelemetryUrl(value)).toBeNull();
  },
);

it.each([
  "javascript:alert(1)",
  "data:text/html,example",
  "file:///synthetic/console.html",
  "https://synthetic:example@example.invalid/console",
])("rejects non-web schemes and embedded credentials: %s", (value) => {
  expect(getTelemetryUrl(value)).toBeNull();
});

it("keeps an explicit HTTPS path and query without adding source identity", () => {
  expect(getTelemetryUrl(" https://example.invalid/console?view=logs ")).toBe(
    "https://example.invalid/console?view=logs",
  );
});

it("accepts an explicitly selected development HTTP tool", () => {
  expect(getTelemetryUrl("http://localhost/console")).toBe(
    "http://localhost/console",
  );
});

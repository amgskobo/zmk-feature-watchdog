import { Incident } from "./proto/cormoran/watchdog/watchdog";
import {
  decodeDiagnostics,
  decodeFatalContext,
  eventNames,
} from "./diagnostics";
import { toHex } from "./incidentHelpers";
import { CaptureContextDetail } from "./CaptureContextDetail";

export function DiagnosticDetail({ incident }: { incident: Incident }) {
  const bytes = incident.diagnostics;
  const diagnostic = decodeDiagnostics(bytes);
  const context = incident.fatal && decodeFatalContext(incident.fatal.context);
  return (
    <>
      <CaptureContextDetail bytes={incident.captureContext} />
      {context && (
        <details>
          <summary>Fatal CPU context</summary>
          <p>
            Exception number (xPSR): {context[5] & 511}. Registers are from
            fatal entry; a software assertion may already have replaced earlier
            values.
          </p>
          <dl>
            {context.map((value, index) => (
              <div key={index}>
                <dt>{["r0", "r1", "r2", "r3", "r12", "xPSR"][index]}</dt>
                <dd>
                  <code>{toHex(value)}</code>
                </dd>
              </div>
            ))}
          </dl>
        </details>
      )}
      {incident.fatal && !context && (
        <small>Fatal CPU context is unavailable in this record.</small>
      )}
      {!bytes.length ? (
        <small>No recent-operation log in this record.</small>
      ) : !diagnostic ? (
        <small>
          Unknown diagnostic format; raw bytes remain in the report.
        </small>
      ) : (
        <details>
          <summary>Recent operations · firmware {diagnostic.buildTag}</summary>
          <p>
            Active profile index:{" "}
            {diagnostic.profile === 255 ? "unknown" : diagnostic.profile}{" "}
            (zero-based BT_SEL index).
          </p>
          <p>
            Cached endpoint:{" "}
            {["None", "USB", "BLE"][diagnostic.endpoint] ?? "unknown"} · USB:{" "}
            {["None", "Powered", "HID ready"][diagnostic.usb] ?? "unknown"}
          </p>
          {diagnostic.observedBle && (
            <p>Observed BLE links: {diagnostic.links}</p>
          )}
          <ol>
            {diagnostic.events.map((entry, index) => (
              <li key={index}>
                <code>{entry.timeMs} ms</code> ·{" "}
                {eventNames[entry.event] ?? `Event ${entry.event}`} ·{" "}
                {entry.event === 20 ? "queue index" : "profile"}{" "}
                {entry.profile === 255 ? "unknown / other" : entry.profile} ·
                value {entry.result} ({toHex(entry.value)})
              </li>
            ))}
          </ol>
          {!!(diagnostic.flags & 1) && (
            <p>Only the latest seven events are retained.</p>
          )}
          {(!!(diagnostic.flags & 2) || !!diagnostic.dropped) && (
            <p>
              Log is incomplete: writer interrupted or events dropped (
              {diagnostic.dropped}). Cached state may be older.
            </p>
          )}
          {!!(diagnostic.flags & 4) && (
            <p>
              CFSR: {toHex(diagnostic.cfsr)} · HFSR: {toHex(diagnostic.hfsr)}
            </p>
          )}
          <small>
            Match this tag to the build manifest and verify its ELF SHA-256
            before decoding addresses. The tag alone is not a unique binary
            identifier. Milliseconds wrap after approximately 49.7 days;
            displayed order is capture order.
          </small>
        </details>
      )}
    </>
  );
}

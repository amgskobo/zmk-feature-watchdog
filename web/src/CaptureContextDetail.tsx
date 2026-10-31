import { decodeCaptureContext } from "./captureContext";
import { toHex } from "./incidentHelpers";

export function CaptureContextDetail({ bytes }: { bytes: Uint8Array }) {
  if (!bytes?.length) return null;
  const context = decodeCaptureContext(bytes);
  if (!context)
    return (
      <p>Unknown common capture format; raw bytes remain in the report.</p>
    );
  return (
    <details>
      <summary>
        Common capture ·{" "}
        {context.timeMs === null ? "time unavailable" : `${context.timeMs} ms`}
      </summary>
      {!!(context.flags & 16) && (
        <p>
          Assertion call site {toHex(context.assertionSite)} · line{" "}
          {context.assertionLine}
        </p>
      )}
      {!!(context.flags & 64) && <p>The interrupted context was an ISR.</p>}
      {!!(context.flags & 32) && (
        <p>CPU register capture is unavailable on this target.</p>
      )}
      {!!(context.flags & 1) && (
        <p>
          Evidence is bounded or partial; dropped metadata operations:{" "}
          {context.dropped}.
        </p>
      )}
      {context.callee && (
        <dl>
          {context.callee.map((value, i) => (
            <div key={i}>
              <dt>r{i + 4}</dt>
              <dd>
                <code>{toHex(value)}</code>
              </dd>
            </div>
          ))}
        </dl>
      )}
      {!!(context.flags & 4) && (
        <p>
          MMFAR {toHex(context.mmfar)} · BFAR {toHex(context.bfar)} · ICSR{" "}
          {toHex(context.icsr)}. Fault-address registers are meaningful only
          with their corresponding CFSR valid bits.
        </p>
      )}
      {!!context.addresses.length && (
        <p>
          Code address candidates: {context.addresses.map(toHex).join(", ")}.
          These are not a complete unwound stack.
        </p>
      )}
      <ol>
        {context.operations.map((operation, i) => (
          <li key={i}>
            {operation.timeMs} ms · sequence {operation.sequence} · domain{" "}
            {operation.domain} / event {operation.event} · arguments{" "}
            {toHex(operation.arg0)}, {toHex(operation.arg1)} · call site{" "}
            {toHex(operation.site)}
          </li>
        ))}
      </ol>
      <small>
        Only four metadata operations are retained. Capture order is preserved
        across timestamp wrap. Use the matching ELF to resolve code addresses.
      </small>
    </details>
  );
}

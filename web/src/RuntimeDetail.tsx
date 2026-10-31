import { StatusResponse, Incident } from "./proto/cormoran/watchdog/watchdog";
import { DiagnosticDetail } from "./DiagnosticDetail";
import { resetCauseLabels, toHex } from "./incidentHelpers";

export function RuntimeDetail({ status }: { status: StatusResponse }) {
  const flags = status.runtimeFlags;
  if (!(flags & 1)) {
    return <p>Runtime identification is unavailable in this older firmware.</p>;
  }
  return (
    <div>
      <p>
        Running firmware: <code>{toHex(status.buildTag).slice(2)}</code>
      </p>
      <p>
        Fatal detector: {flags & 2 ? "enabled" : "disabled"} · System queue
        monitor: {!(flags & 4) ? "disabled" : flags & 8 ? "armed" : "not armed"}
        {flags & 16 ? " · Low-priority monitor armed" : ""}
        {flags & 32 ? " · Monitor arming failed" : ""}
      </p>
      {!!(flags & 512) && (
        <p>
          Software watchdog only. A CPU or timer interrupt stop can prevent
          automatic restart.
        </p>
      )}
      <p>
        Boot reset cause:{" "}
        {flags & 64
          ? `${toHex(status.resetCause)} (${resetCauseLabels(status.resetCause).join(", ") || "no recognized bits"})`
          : "unavailable"}
      </p>
      {!!(flags & 256) && <p>A retained checkpoint failed validation.</p>}
      {!!(flags & 1024) && (
        <p>A pending incident is retained; persistence is not finished.</p>
      )}
      {!!(flags & 2048) && (
        <p>A pending incident failed validation and was discarded.</p>
      )}
      {!!(flags & 4096) && (
        <p>Incident persistence failed; the retained record will be retried.</p>
      )}
      {!!(flags & 8192) && <p>Reset-cause persistence or clearing failed.</p>}
      <p>
        Common capture: {flags & 16384 ? "enabled" : "disabled"} · Assertion
        call-site capture: {flags & 32768 ? "enabled" : "disabled"}
      </p>
      {status.previousDiagnostics.length ? (
        <>
          <p>
            Before this boot: last completed log entries recovered from RAM.
            These are separate from stored incidents and do not identify a crash
            or its CPU address.
          </p>
          <DiagnosticDetail
            incident={Incident.create({
              diagnostics: status.previousDiagnostics,
            })}
          />
        </>
      ) : (
        <p>No operation checkpoint recovered from before this boot.</p>
      )}
    </div>
  );
}

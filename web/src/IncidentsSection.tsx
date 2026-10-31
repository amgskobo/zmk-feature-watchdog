import {
  useContext,
  useEffect,
  useState,
  useCallback,
  useRef,
  useMemo,
} from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  Incident,
  Notification,
  StatusResponse,
} from "./proto/cormoran/watchdog/watchdog";
import {
  incidentDetailSummary,
  incidentTypeLabel,
  sourceLabel,
  toHex,
} from "./incidentHelpers";
import { decodeFreezeTrace } from "./freezeTrace";
import { DiagnosticDetail } from "./DiagnosticDetail";
import { RuntimeDetail } from "./RuntimeDetail";
import { CLIENT_REVISION } from "./reportFormat";
import { callWithRelay } from "./relayRequest";
import { serializeRpc } from "./serializedRpc";
import { getTelemetryUrl } from "./telemetryUrl";

export const SUBSYSTEM_IDENTIFIER = "cormoran__watchdog";

function TraceDetail({ incident }: { incident: Incident }) {
  if (!incident.freeze) return null;
  const bytes = incident.freeze.trace;
  if (!bytes?.length) return <small>No trace in this older record.</small>;
  const trace = decodeFreezeTrace(bytes);
  if (!trace)
    return (
      <small>Unrecognized trace format; raw bytes remain in the report.</small>
    );
  return (
    <details>
      <summary>Freeze trace ({trace.addresses.length} code addresses)</summary>
      <div>
        Thread state: {toHex(trace.state)} · flags: {toHex(trace.flags)}
      </div>
      <div>Wait queue: {toHex(trace.waitQueue)}</div>
      <div>Stack pointer: {toHex(trace.stackPointer)}</div>
      {(trace.flags & 16) !== 0 && (
        <p>Stack capture unavailable on this target or configuration.</p>
      )}
      <ol>
        {trace.addresses.map((address, index) => (
          <li key={index}>
            {index === 0
              ? (trace.flags & 2) !== 0
                ? "Interrupted PC: "
                : "Saved PC: "
              : "Return candidate: "}
            <code>{toHex(address)}</code>
          </li>
        ))}
      </ol>
      <small>
        Return candidates are not a complete unwound stack. Decode with the ELF
        of the firmware that recorded this incident.
      </small>
      {(trace.flags & 8) !== 0 && (
        <p>Trace is partial or reached its capture limit.</p>
      )}
    </details>
  );
}

const PAGE_SIZE = 4;

// How long to wait for a peripheral to answer a relayed request (DESIGN.md
// SS7) before giving up -- there is no way to know which peripheral slots
// are currently connected, so a relayed request to a disconnected/missing
// peripheral would otherwise hang forever.
const RELAY_TIMEOUT_MS = 3000;
const RPC_TIMEOUT_MS = 15000;

export function IncidentsSection() {
  const telemetryUrl = getTelemetryUrl();
  const zmkApp = useContext(ZMKAppContext);
  // 0 = central/local; >0 = split peripheral
  // slot N, relayed over CONFIG_ZMK_SPLIT_RELAY_EVENT (DESIGN.md, Studio RPC, split relay and client). There
  // is no discovery of how many peripherals are connected, so this offers a
  // small fixed range -- enough for the common single/dual-peripheral split.
  const [source, setSource] = useState(0);
  const [status, setStatus] = useState<StatusResponse | null>(null);
  const [incidents, setIncidents] = useState<Incident[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [confirmingDeleteAll, setConfirmingDeleteAll] = useState(false);
  const [reportCopied, setReportCopied] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);
  const [copyError, setCopyError] = useState<string | null>(null);
  const generation = useRef(0);
  const requestAbort = useRef<AbortController | null>(null);
  const [retrieval, setRetrieval] = useState({
    complete: false,
    error: null as string | null,
    received: 0,
    expected: null as number | null,
  });

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER) ?? null;

  const callRpc = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!zmkApp?.state.connection || !subsystem) return null;
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) return null;
      return Response.decode(responsePayload);
    },
    [zmkApp, subsystem]
  );

  const callRpcWithRelay = useCallback(
    async (
      request: Request,
      signal?: AbortSignal
    ): Promise<Response | null> => {
      if (!zmkApp || !subsystem) return null;
      const response = await callWithRelay(
        () => callRpc(request),
        (callback) =>
          zmkApp.onNotification({
            type: "custom",
            subsystemIndex: subsystem.index,
            callback: (value) => {
              try {
                callback(Notification.decode(value.payload));
              } catch {
                /* Ignore malformed broadcasts. */
              }
            },
          }),
        source,
        RELAY_TIMEOUT_MS,
        signal,
        RPC_TIMEOUT_MS
      );
      if (!response)
        throw new Error(
          `Source ${source} did not respond before the RPC/relay deadline`
        );
      return response;
    },
    [callRpc, zmkApp, subsystem, source]
  );

  const callRpcAwaitingRelay = useMemo(
    () => serializeRpc(callRpcWithRelay),
    [callRpcWithRelay]
  );
  const refreshAll = useCallback(async () => {
    requestAbort.current?.abort();
    const controller = new AbortController();
    requestAbort.current = controller;
    const token = ++generation.current;
    const active = () =>
      token === generation.current && !controller.signal.aborted;
    const collected: Incident[] = [];
    let expected: number | null = null;
    setIsLoading(true);
    setError(null);
    setCopyError(null);
    setReportCopied(false);
    setRetrieval({ complete: false, error: null, received: 0, expected: null });
    try {
      const statusResponse = await callRpcAwaitingRelay(
        Request.create({ getStatus: { source } }),
        controller.signal
      );
      if (!active()) return;
      if (!statusResponse?.status)
        throw new Error(statusResponse?.error?.message ?? "No status response");
      const loadedStatus = statusResponse.status;
      if (
        loadedStatus.stored > loadedStatus.capacity ||
        loadedStatus.capacity > 65535
      )
        throw new Error("Invalid store bounds");
      expected = loadedStatus.stored;
      setStatus(loadedStatus);
      let startIndex = 0;
      do {
        const response = await callRpcAwaitingRelay(
          Request.create({ listIncidents: { startIndex, source } }),
          controller.signal
        );
        if (!active()) return;
        const page = response?.incidentPage;
        if (!page)
          throw new Error(response?.error?.message ?? "No incident page");
        if (
          page.startIndex !== startIndex ||
          page.total !== expected ||
          page.incidents.length > PAGE_SIZE ||
          page.incidents.some((incident) => incident.source !== source) ||
          startIndex + page.incidents.length > expected
        )
          throw new Error(
            "Incident page changed or does not match this source; refresh"
          );
        collected.push(...page.incidents);
        startIndex += page.incidents.length;
        if (startIndex === expected) break;
        if (page.incidents.length === 0)
          throw new Error("Incomplete incident page; refresh");
      } while (startIndex < expected);
      setIncidents(collected);
      setRetrieval({
        complete: true,
        error: null,
        received: collected.length,
        expected,
      });
    } catch (err) {
      if (!active()) return;
      const message =
        err instanceof Error ? err.message : "Failed to load incidents";
      setIncidents(collected);
      setError(message);
      setRetrieval({
        complete: false,
        error: message,
        received: collected.length,
        expected,
      });
    } finally {
      if (active()) setIsLoading(false);
    }
  }, [callRpcAwaitingRelay, source]);

  const retireRequests = useCallback(() => {
    ++generation.current;
    requestAbort.current?.abort();
  }, []);

  // External transport/source changes invalidate displayed evidence. Keep this
  // reset immediate so no previous device's records appear as current.
  /* eslint-disable react-hooks/set-state-in-effect */
  useEffect(() => {
    if (!subsystem || !zmkApp?.state.connection) {
      retireRequests();
      setStatus(null);
      setIncidents([]);
      setIsLoading(false);
      setError(null);
      setActionError(null);
      setCopyError(null);
      setReportCopied(false);
      setConfirmingDeleteAll(false);
      setRetrieval({
        complete: false,
        error: null,
        received: 0,
        expected: null,
      });
      return;
    }

    setStatus(null);
    setIncidents([]);
    setConfirmingDeleteAll(false);
    setActionError(null);
    void refreshAll();
    return retireRequests;
    // Re-run when the connected watchdog subsystem becomes available, or
    // when the user switches source (central/peripheral).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index, zmkApp?.state.connection, source, retireRequests]);
  /* eslint-enable react-hooks/set-state-in-effect */

  useEffect(() => {
    if (!zmkApp || !subsystem) return;

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (customNotification) => {
        let notification: Notification;
        try {
          notification = Notification.decode(customNotification.payload);
        } catch {
          return;
        }
        // Only IncidentRecorded is a live/spontaneous push -- it always
        // describes this device's own local store (source 0), matching
        // PeripheralResponse notifications are
        // consumed by callWithRelay() (they are answers to
        // a specific requestId, not a general live-update stream) and
        // intentionally ignored here.
        if (notification.incidentRecorded?.incident) {
          if (source !== 0) return;
          void refreshAll();
        }
      },
    });

    return unsubscribe;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zmkApp, subsystem?.index, source]);

  const performDelete = async (request: Request) => {
    requestAbort.current?.abort();
    const controller = new AbortController();
    requestAbort.current = controller;
    const token = generation.current;
    const active = () =>
      token === generation.current && !controller.signal.aborted;
    setIsLoading(true);
    setActionError(null);
    try {
      const response = await callRpcAwaitingRelay(request, controller.signal);
      if (!active()) return;
      if (response?.error) setActionError(response.error.message);
      setConfirmingDeleteAll(false);
      await refreshAll();
    } catch (err) {
      if (active())
        setActionError(err instanceof Error ? err.message : "Deletion failed");
    } finally {
      if (active()) setIsLoading(false);
    }
  };
  const deleteOne = (id: number) =>
    performDelete(
      Request.create({ deleteIncidents: { ids: [id], all: false, source } })
    );
  const deleteAll = () =>
    performDelete(
      Request.create({ deleteIncidents: { ids: [], all: true, source } })
    );

  const copyReport = async () => {
    const token = generation.current;
    setReportCopied(false);
    setCopyError(null);
    try {
      const report = {
        format: "zmk-watchdog-report-v1",
        clientRevision: CLIENT_REVISION,
        source,
        status: status && {
          ...status,
          previousDiagnostics: Array.from(status.previousDiagnostics),
        },
        copiedAt: new Date().toISOString(),
        retrieval,
        note: "Use the matching firmware ELF for each incident; stack addresses are candidates.",
        incidents: incidents.map((incident) => ({
          ...incident,
          diagnostics: Array.from(incident.diagnostics ?? []),
          captureContext: Array.from(incident.captureContext ?? []),
          ...(incident.fatal
            ? {
                fatal: {
                  ...incident.fatal,
                  context: Array.from(incident.fatal.context ?? []),
                },
              }
            : {}),
          ...(incident.freeze
            ? {
                freeze: {
                  ...incident.freeze,
                  trace: Array.from(incident.freeze.trace ?? []),
                },
              }
            : {}),
        })),
      };
      await navigator.clipboard.writeText(JSON.stringify(report, null, 2));
      if (token === generation.current) setReportCopied(true);
    } catch (err) {
      if (token !== generation.current) return;
      setCopyError(
        err instanceof Error ? err.message : "Could not copy diagnostic report"
      );
    }
  };

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the watchdog module.
          </p>
        </div>
      </section>
    );
  }

  // A fixed small range -- there is no RPC to discover how many peripherals
  // are connected (DESIGN.md, Studio RPC, split relay and client), so this just offers "Central" plus a
  // handful of peripheral slots. Selecting a peripheral re-fetches status
  // and incidents relayed from that half's own local store.
  const sourceOptions = [0, 1, 2, 3];

  return (
    <>
      <section className="card">
        <h2>Fault and reset diagnostics</h2>
        <p>
          Read reset causes, retained incidents and the last recorded
          operations.
        </p>
        <div className="source-selector">
          <label htmlFor="watchdog-source-select">
            <strong>Source:</strong>
          </label>
          <select
            id="watchdog-source-select"
            value={source}
            disabled={isLoading}
            onChange={(e) => setSource(Number(e.target.value))}
          >
            {sourceOptions.map((s) => (
              <option key={s} value={s}>
                {sourceLabel(s)}
              </option>
            ))}
          </select>
        </div>
        <p>
          Each half's USB console is separate from central Studio access.
          {telemetryUrl && (
            <>
              {" "}
              <a href={telemetryUrl} target="_blank" rel="noopener noreferrer">
                Open USB telemetry
              </a>
              .
            </>
          )}
        </p>
      </section>
      <section className="card">
        <h2>Status</h2>
        {source !== 0 && (
          <div className="warning-message">
            <p>
              ℹ️ Peripheral incidents are relayed over the split link and may
              take a few seconds, or time out if the peripheral is disconnected
              (see README).
            </p>
          </div>
        )}
        {error && (
          <div className="error-message">
            <p>🚨 {error}</p>
          </div>
        )}
        {status ? (
          <div className="status-grid">
            <div>
              <strong>Capacity:</strong> {status.capacity}
            </div>
            <div>
              <strong>Stored:</strong> {status.stored}
            </div>
            <div>
              <strong>Dropped since boot:</strong> {status.droppedSinceBoot}
            </div>
          </div>
        ) : (
          <p>{isLoading ? "⏳ Loading..." : "No status loaded yet."}</p>
        )}
        {status?.recordingStopped && (
          <div className="warning-message">
            <p>
              ⚠️ Log full — recording paused. Delete incidents below to resume
              recording.
            </p>
          </div>
        )}
        {status && <RuntimeDetail status={status} />}
        <button
          className="btn btn-secondary"
          disabled={isLoading}
          onClick={refreshAll}
        >
          {isLoading ? "⏳ Refreshing..." : "🔄 Refresh"}
        </button>
      </section>

      <section className="card">
        <h2>Incidents ({incidents.length})</h2>
        <button
          className="btn btn-secondary"
          disabled={isLoading}
          onClick={copyReport}
        >
          Copy diagnostic report
        </button>
        {reportCopied && <span role="status">Report copied</span>}
        {copyError && <p role="alert">{copyError}</p>}
        {actionError && <p role="alert">{actionError}</p>}
        {retrieval.error && retrieval.received > 0 && (
          <p role="alert">
            Partial report: {retrieval.received} of {retrieval.expected}{" "}
            incidents loaded.
          </p>
        )}
        {incidents.length === 0 ? (
          <p>
            {retrieval.complete
              ? "No incidents recorded."
              : isLoading
                ? "Loading incidents..."
                : "No incident records loaded."}
          </p>
        ) : (
          <div className="table-scroll">
            <table className="incidents-table">
              <thead>
                <tr>
                  <th>ID</th>
                  <th>Source</th>
                  <th>Type</th>
                  <th>Incident ordinal / Uptime</th>
                  <th>Detail</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {incidents.map((incident, index) => (
                  <tr key={`${index}-${incident.id}`}>
                    <td>{incident.id}</td>
                    <td>{sourceLabel(incident.source)}</td>
                    <td>
                      <span
                        className={`badge badge-${incidentTypeLabel(
                          incident.type
                        )
                          .toLowerCase()
                          .replace(/\s+/g, "-")}`}
                      >
                        {incidentTypeLabel(incident.type)}
                      </span>
                    </td>
                    <td>
                      #{incident.bootOrdinal} @{" "}
                      {incident.captureFlags & 2
                        ? "unavailable (minimal capture)"
                        : `${incident.uptimeS}s`}
                    </td>
                    <td className="detail-cell">
                      {incident.captureFlags & 2 ? (
                        <p>
                          Capture incomplete; only a minimal explanation was
                          committed.
                        </p>
                      ) : null}
                      {incidentDetailSummary(incident)}
                      <TraceDetail incident={incident} />
                      <DiagnosticDetail incident={incident} />
                    </td>
                    <td>
                      <button
                        className="btn btn-danger"
                        disabled={isLoading}
                        onClick={() => deleteOne(incident.id)}
                      >
                        🗑️ Delete
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}

        {incidents.length > 0 && (
          <div className="delete-all-row">
            {confirmingDeleteAll ? (
              <>
                <span>Delete all {incidents.length} incidents?</span>
                <button
                  className="btn btn-danger"
                  disabled={isLoading}
                  onClick={deleteAll}
                >
                  Confirm delete all
                </button>
                <button
                  className="btn btn-secondary"
                  disabled={isLoading}
                  onClick={() => setConfirmingDeleteAll(false)}
                >
                  Cancel
                </button>
              </>
            ) : (
              <button
                className="btn btn-danger"
                disabled={isLoading}
                onClick={() => setConfirmingDeleteAll(true)}
              >
                🗑️ Delete All
              </button>
            )}
          </div>
        )}
      </section>
    </>
  );
}

// Re-exported for tests that want to reference the configured page size.
export const WATCHDOG_RPC_PAGE_SIZE = PAGE_SIZE;

import { StrictMode } from "react";
import { act, fireEvent, render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";
import {
  IncidentsSection,
  SUBSYSTEM_IDENTIFIER,
} from "../../web/src/IncidentsSection";
import {
  Incident,
  Notification,
  Request,
  Response,
} from "../../web/src/proto/cormoran/watchdog/watchdog";

// Keep the pinned ZMKCustomSubsystem/withTimeout implementation real. Only
// the lower transport result is controlled, so its default 5s timer matters.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));
function reply(value: Parameters<typeof Response.create>[0]) {
  return {
    custom: {
      call: { payload: Response.encode(Response.create(value)).finish() },
    },
  };
}
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
function fixture() {
  const app = createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });
  type Subscription = Parameters<typeof app.onNotification>[0];
  const listeners = new Set<Subscription>();
  app.onNotification = jest.fn((subscription: Subscription) => {
    listeners.add(subscription);
    return () => {
      listeners.delete(subscription);
    };
  });
  return {
    app,
    core(state: LockState) {
      for (const listener of [...listeners]) {
        if (listener.type === "core")
          listener.callback({ lockStateChanged: state });
      }
    },
    custom(value: Parameters<typeof Notification.create>[0]) {
      const payload = Notification.encode(Notification.create(value)).finish();
      for (const listener of [...listeners]) {
        if (listener.type === "custom")
          listener.callback({ subsystemIndex: 0, payload });
      }
    },
    customListeners: () =>
      [...listeners].filter((listener) => listener.type === "custom").length,
  };
}
function panel(value: ReturnType<typeof fixture>, strict = false) {
  const content = (
    <ZMKAppProvider value={value.app}>
      <IncidentsSection />
    </ZMKAppProvider>
  );
  return strict ? <StrictMode>{content}</StrictMode> : content;
}
async function flush() {
  await act(async () => {
    for (let n = 0; n < 32; n++) await Promise.resolve();
  });
}
async function loaded() {
  await act(async () => {
    await jest.advanceTimersByTimeAsync(0);
  });
  await flush();
}
function requests() {
  return (call_rpc as jest.Mock).mock.calls.map((call) =>
    Request.decode(call[1].custom.call.payload),
  );
}
const LOCKED = LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED;
const UNLOCKED = LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;

describe("incident authorization and pinned-SDK deadlines", () => {
  let records: Incident[];
  let intercept: ((request: Request) => unknown) | undefined;
  beforeEach(() => {
    jest.useFakeTimers();
    jest.clearAllMocks();
    records = [
      Incident.create({
        id: 101,
        source: 0,
        freeze: { queueName: "old-queue" },
      }),
    ];
    intercept = undefined;
    (call_rpc as jest.Mock).mockImplementation((_connection, raw) => {
      const request = Request.decode(raw.custom.call.payload);
      const blocked = intercept?.(request);
      if (blocked !== undefined) return blocked;
      return Promise.resolve(
        reply(
          request.getStatus
            ? { status: { capacity: 16, stored: records.length } }
            : request.listIncidents
              ? { incidentPage: { total: records.length, incidents: records } }
              : { deleteResult: { deleted: 1 } },
        ),
      );
    });
  });
  afterEach(() => {
    jest.clearAllTimers();
    jest.useRealTimers();
  });

  it("clears evidence on lock and never revives a retired page after unlock", async () => {
    const current = fixture();
    render(panel(current, true));
    await loaded();
    expect(screen.getByText(/old-queue/)).toBeInTheDocument();
    const pending = deferred<ReturnType<typeof reply>>();
    intercept = (request) =>
      request.listIncidents ? pending.promise : undefined;
    fireEvent.click(screen.getByRole("button", { name: "🔄 Refresh" }));
    await flush();
    expect(requests().filter((request) => request.listIncidents)).toHaveLength(
      2,
    );
    await act(async () => {
      current.core(LOCKED);
    });
    expect(screen.getByText(/Studio is locked/)).toBeInTheDocument();
    expect(screen.queryByText(/old-queue/)).not.toBeInTheDocument();
    expect(
      screen.queryByRole("button", { name: "Copy diagnostic report" }),
    ).not.toBeInTheDocument();
    expect(current.customListeners()).toBe(0);
    records = [
      Incident.create({
        id: 202,
        source: 0,
        freeze: { queueName: "new-queue" },
      }),
    ];
    intercept = undefined;
    await act(async () => {
      current.core(UNLOCKED);
    });
    pending.resolve(
      reply({
        incidentPage: {
          total: 1,
          incidents: [
            Incident.create({
              id: 101,
              source: 0,
              freeze: { queueName: "old-queue" },
            }),
          ],
        },
      }),
    );
    await loaded();
    expect(screen.getByText(/new-queue/)).toBeInTheDocument();
    expect(screen.queryByText(/old-queue/)).not.toBeInTheDocument();
    expect(screen.getByRole("button", { name: "🗑️ Delete" })).toBeEnabled();
  });

  it("retires a pending deletion across a batched lock/unlock without refreshing its old view", async () => {
    const current = fixture();
    render(panel(current));
    await loaded();
    const pending = deferred<ReturnType<typeof reply>>();
    intercept = (request) =>
      request.deleteIncidents ? pending.promise : undefined;
    fireEvent.click(screen.getByRole("button", { name: "🗑️ Delete" }));
    await flush();
    expect(
      requests().filter((request) => request.deleteIncidents),
    ).toHaveLength(1);
    records = [];
    await act(async () => {
      current.core(LOCKED);
      current.core(UNLOCKED);
    });
    await loaded();
    const freshCount = requests().length;
    pending.resolve(reply({ deleteResult: { deleted: 1 } }));
    await flush();
    expect(requests()).toHaveLength(freshCount);
    expect(screen.getByText("No incidents recorded.")).toBeInTheDocument();
    expect(screen.queryByText(/old-queue/)).not.toBeInTheDocument();
  });

  it("cancels old relay listeners and ignores their answer during a new source request", async () => {
    const current = fixture();
    records = [];
    let id = 40;
    intercept = (request) =>
      request.getStatus?.source === 1
        ? Promise.resolve(reply({ deferred: { requestId: ++id } }))
        : undefined;
    render(panel(current));
    await loaded();
    fireEvent.change(screen.getByLabelText("Source:"), {
      target: { value: "1" },
    });
    await loaded();
    expect(id).toBe(41);
    await act(async () => {
      current.core(LOCKED);
    });
    expect(current.customListeners()).toBe(0);
    await act(async () => {
      current.core(UNLOCKED);
    });
    await loaded();
    fireEvent.change(screen.getByLabelText("Source:"), {
      target: { value: "1" },
    });
    await loaded();
    expect(id).toBe(42);
    await act(async () => {
      current.custom({
        peripheralResponse: {
          source: 1,
          requestId: 41,
          response: { status: { capacity: 99, stored: 0 } },
        },
      });
    });
    expect(
      screen.getByRole("button", { name: "⏳ Refreshing..." }),
    ).toBeDisabled();
    expect(screen.queryByText("99")).not.toBeInTheDocument();
    await act(async () => {
      current.custom({
        peripheralResponse: {
          source: 1,
          requestId: 42,
          response: { status: { capacity: 16, stored: 0 } },
        },
      });
    });
    await flush();
    expect(screen.getByRole("button", { name: "🔄 Refresh" })).toBeEnabled();
    expect(screen.getByText("No incidents recorded.")).toBeInTheDocument();
  });

  it("accepts a six-second reply through the real pinned SDK", async () => {
    const pending = deferred<ReturnType<typeof reply>>();
    records = [];
    intercept = (request) => (request.getStatus ? pending.promise : undefined);
    render(panel(fixture()));
    await loaded();
    await act(async () => {
      await jest.advanceTimersByTimeAsync(6000);
    });
    expect(
      screen.getByRole("button", { name: "⏳ Refreshing..." }),
    ).toBeDisabled();
    expect(screen.queryByText(/deadline|timed out/i)).not.toBeInTheDocument();
    pending.resolve(reply({ status: { capacity: 16, stored: 0 } }));
    await flush();
    expect(screen.getByText("No incidents recorded.")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "🔄 Refresh" })).toBeEnabled();
  });

  it("expires an unresolved RPC at fifteen seconds, ignores its late reply and allows retry", async () => {
    const pending = deferred<ReturnType<typeof reply>>();
    records = [];
    intercept = (request) => (request.getStatus ? pending.promise : undefined);
    render(panel(fixture()));
    await loaded();
    await act(async () => {
      await jest.advanceTimersByTimeAsync(14999);
    });
    expect(
      screen.getByRole("button", { name: "⏳ Refreshing..." }),
    ).toBeDisabled();
    await act(async () => {
      await jest.advanceTimersByTimeAsync(1);
    });
    expect(screen.getByRole("button", { name: "🔄 Refresh" })).toBeEnabled();
    expect(screen.getAllByText(/deadline|timed out/i).length).toBeGreaterThan(
      0,
    );
    pending.resolve(reply({ status: { capacity: 99, stored: 0 } }));
    await flush();
    expect(screen.queryByText("99")).not.toBeInTheDocument();
    intercept = undefined;
    fireEvent.click(screen.getByRole("button", { name: "🔄 Refresh" }));
    await flush();
    expect(screen.getByText("No incidents recorded.")).toBeInTheDocument();
  });
});

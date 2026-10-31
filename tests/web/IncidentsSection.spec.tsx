import {
  render,
  screen,
  waitFor,
  fireEvent,
  act,
} from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  createMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import {
  IncidentsSection,
  SUBSYSTEM_IDENTIFIER,
} from "../../web/src/IncidentsSection";
import { sourceLabel } from "../../web/src/incidentHelpers";
import { ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";
import {
  Incident,
  Request,
  Response,
} from "../../web/src/proto/cormoran/watchdog/watchdog";

it("has no retired measurement UI or background health requests", async () => {
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementation(async (payload) => {
      const request = Request.decode(payload);
      return Response.encode(
        Response.create(
          request.getStatus
            ? { status: { capacity: 16, stored: 0 } }
            : { incidentPage: { total: 0, incidents: [] } },
        ),
      ).finish();
    });
  try {
    const app = createConnectedMockZMKApp({
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });
    render(
      <ZMKAppProvider value={app}>
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await waitFor(() => expect(rpc).toHaveBeenCalled());
    expect(
      screen.queryByRole("heading", { name: "System observation" }),
    ).not.toBeInTheDocument();
    expect(
      screen.queryByRole("button", { name: /older firmware observations/i }),
    ).not.toBeInTheDocument();
    // GetHealth is reserved and no longer generated: every request the UI
    // sends must decode to one of the current request kinds.
    expect(
      rpc.mock.calls.every(([payload]) => {
        const request = Request.decode(payload);
        return Boolean(
          request.getStatus ||
          request.listIncidents ||
          request.deleteIncidents ||
          request.injectTest,
        );
      }),
    ).toBe(true);
  } finally {
    rpc.mockRestore();
  }
});

it("shows new trace details, retains old records and copies ordered raw evidence", async () => {
  const trace = Uint8Array.from([
    1,
    2,
    5,
    2,
    0x24,
    1,
    0,
    0x20,
    0,
    0x10,
    0,
    0x20,
    0x20,
    0x10,
    0,
    0,
    0x31,
    0x20,
    0,
    0,
    ...Array(16).fill(0),
  ]);
  const diagnostics = new Uint8Array(80);
  diagnostics.set([1, 1, 0, 1]);
  new DataView(diagnostics.buffer).setUint32(4, 0x12345678, true);
  diagnostics.set([3, 2, 0, 1], 8);
  new DataView(diagnostics.buffer).setUint32(24, 139000, true);
  diagnostics.set([10, 3, 0, 0], 28);
  const context = new Uint8Array(24);
  new DataView(context.buffer).setUint32(20, 0x1000011, true);
  const captureContext = new Uint8Array(192);
  captureContext.set([1, 1 | 16, 0, 1]);
  const capture = new DataView(captureContext.buffer);
  capture.setUint32(8, 0x1235, true);
  capture.setUint32(12, 674, true);
  capture.setUint32(96, 1, true);
  capture.setUint32(112, 0xffffffff, true);
  const records = [
    Incident.create({
      id: 7,
      bootOrdinal: 2,
      uptimeS: 10,
      freeze: { queueName: "sysworkq", trace },
      diagnostics,
    }),
    Incident.create({
      id: 1,
      bootOrdinal: 1,
      uptimeS: 975,
      freeze: { queueName: "sysworkq" },
    }),
    Incident.create({
      id: 6,
      type: 1,
      bootOrdinal: 6,
      uptimeS: 139,
      fatal: { reason: 3, pc: 337522, lr: 337507, threadName: "?", context },
      diagnostics,
      captureContext,
    }),
  ];
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementation(async (payload) => {
      const request = Request.decode(payload);
      return Response.encode(
        Response.create(
          request.getStatus
            ? { status: { capacity: 16, stored: 3 } }
            : { incidentPage: { total: 3, incidents: records } },
        ),
      ).finish();
    });
  const writeText = jest.fn().mockResolvedValue(undefined);
  Object.defineProperty(navigator, "clipboard", {
    configurable: true,
    value: { writeText },
  });
  try {
    const mockZMKApp = createConnectedMockZMKApp({
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });
    render(
      <ZMKAppProvider value={mockZMKApp}>
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await screen.findByText("Freeze trace (2 code addresses)");
    expect(screen.getByText(/Wait queue: 0x20000124/)).toBeInTheDocument();
    expect(
      screen.getByText("No trace in this older record."),
    ).toBeInTheDocument();
    fireEvent.click(
      screen.getByRole("button", { name: "Copy diagnostic report" }),
    );
    await screen.findByText("Report copied");
    const report = JSON.parse(writeText.mock.calls[0][0]);
    expect(report.format).toBe("zmk-watchdog-report-v1");
    expect(report.clientRevision).toBe("2026-10-31.publication1");
    expect(report.retrieval).toEqual({
      complete: true,
      error: null,
      received: 3,
      expected: 3,
    });
    expect(Number.isNaN(Date.parse(report.copiedAt))).toBe(false);
    expect(
      report.incidents.map((i: Incident) => [i.id, i.bootOrdinal, i.uptimeS]),
    ).toEqual([
      [7, 2, 10],
      [1, 1, 975],
      [6, 6, 139],
    ]);
    expect(report.incidents[0].freeze.trace).toEqual(Array.from(trace));
    expect(report.incidents[1].freeze.trace).toEqual([]);
    expect(report.incidents[0].diagnostics).toEqual(Array.from(diagnostics));
    expect(report.incidents[2].fatal.context).toEqual(Array.from(context));
    expect(report.incidents[2].captureContext).toEqual(
      Array.from(captureContext),
    );
    expect(report.incidents[1].captureContext).toEqual([]);
    expect(screen.getByText("Fatal CPU context")).toBeInTheDocument();
    expect(screen.getAllByText(/BT_CLR begin/)).toHaveLength(2);
    expect(
      screen.getByText(/Runtime identification is unavailable/),
    ).toBeInTheDocument();
    writeText.mockRejectedValueOnce(new Error("clipboard unavailable"));
    fireEvent.click(
      screen.getByRole("button", { name: "Copy diagnostic report" }),
    );
    await screen.findByText(/clipboard unavailable/);
    fireEvent.click(
      screen.getByRole("button", { name: "Copy diagnostic report" }),
    );
    await screen.findByText("Report copied");
    expect(JSON.parse(writeText.mock.calls[2][0]).retrieval).toEqual(
      report.retrieval,
    );
  } finally {
    rpc.mockRestore();
  }
});

it("reports the running build and recovered boot evidence independently of stored incidents", async () => {
  const previous = new Uint8Array(80);
  previous.set([1, 1, 0, 1]);
  new DataView(previous.buffer).setUint32(4, 0x12345678, true);
  previous.set([2, 0, 0, 1], 8);
  new DataView(previous.buffer).setUint32(24, 0xfffffff0, true);
  previous.set([20, 0, 0xf4, 0xff], 28); // queue zero arm failed with -12
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementation(async (payload) => {
      const request = Request.decode(payload);
      return Response.encode(
        Response.create(
          request.getStatus
            ? {
                status: {
                  capacity: 16,
                  stored: 0,
                  buildTag: 0x87654321,
                  runtimeFlags: 1 | 2 | 4 | 8 | 64 | 128 | 512,
                  resetCause: 1,
                  previousDiagnostics: previous,
                },
              }
            : { incidentPage: { total: 0 } },
        ),
      ).finish();
    });
  const writeText = jest.fn().mockResolvedValue(undefined);
  Object.defineProperty(navigator, "clipboard", {
    configurable: true,
    value: { writeText },
  });
  try {
    render(
      <ZMKAppProvider
        value={createConnectedMockZMKApp({
          subsystems: [SUBSYSTEM_IDENTIFIER],
        })}
      >
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await screen.findByText("87654321");
    expect(screen.getByText(/System queue monitor: armed/)).toBeInTheDocument();
    expect(screen.getByText(/0x00000001 \(PIN\)/)).toBeInTheDocument();
    expect(
      screen.getByText("Recent operations · firmware 12345678"),
    ).toBeInTheDocument();
    expect(screen.getByText(/Monitor arm result/)).toHaveTextContent(
      "value -12",
    );
    expect(screen.getByText(/Software watchdog only/)).toBeInTheDocument();
    fireEvent.click(
      screen.getByRole("button", { name: "Copy diagnostic report" }),
    );
    await screen.findByText("Report copied");
    const report = JSON.parse(writeText.mock.calls[0][0]);
    expect(report.retrieval.complete).toBe(true);
    expect(report.status.buildTag).toBe(0x87654321);
    expect(report.status.resetCause).toBe(1);
    expect(report.status.previousDiagnostics).toEqual(Array.from(previous));
    expect(report.incidents).toEqual([]);
  } finally {
    rpc.mockRestore();
  }
});

describe("IncidentsSection Component", () => {
  describe("With Subsystem", () => {
    it("should render the status card and attempt to load data", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>,
      );

      expect(
        screen.getByRole("heading", { name: "Status" }),
      ).toBeInTheDocument();

      // The mock connection has no working transport, so the RPC call
      // rejects and the component should surface that as an error rather
      // than crash.
      await waitFor(() => {
        expect(screen.getByText(/🚨/i)).toBeInTheDocument();
      });
    });

    it("should distinguish loading from an empty completed report", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>,
      );

      expect(screen.getByText(/Incidents \(0\)/i)).toBeInTheDocument();
      expect(screen.getByText(/Loading incidents/i)).toBeInTheDocument();
      await screen.findByText(/No incident records loaded/i);
    });

    it("should offer a source selector defaulting to Central, and show the peripheral-relay hint when switched (DESIGN.md, Studio RPC, split relay and client)", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>,
      );

      const select = screen.getByLabelText(/Source/i) as HTMLSelectElement;
      expect(select.value).toBe("0");
      expect(
        screen.queryByText(/Peripheral incidents are relayed/i),
      ).not.toBeInTheDocument();

      fireEvent.change(select, { target: { value: "1" } });

      expect(select.value).toBe("1");
      expect(screen.getByText(sourceLabel(1))).toBeInTheDocument();
      await waitFor(() => {
        expect(
          screen.getByText(/Peripheral incidents are relayed/i),
        ).toBeInTheDocument();
      });
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>,
      );

      expect(
        screen.getByText(/Subsystem "cormoran__watchdog" not found/i),
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the watchdog module/i,
        ),
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<IncidentsSection />);

      expect(container.firstChild).toBeNull();
    });
  });

  describe("Disconnected app", () => {
    it("should not attempt to load data without a connection", () => {
      const mockZMKApp = createMockZMKApp({
        findSubsystem: () => ({ index: 0, identifier: SUBSYSTEM_IDENTIFIER }),
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>,
      );

      expect(
        screen.getByRole("heading", { name: "Status" }),
      ).toBeInTheDocument();
      expect(screen.getByText(/No status loaded yet/i)).toBeInTheDocument();
    });
  });
});

it("preserves an ordered partial page, duplicate IDs and minimal-capture evidence", async () => {
  const records = [10, 20, 30, 40].map((uptimeS, index) =>
    Incident.create({
      id: 7,
      source: 0,
      uptimeS,
      captureFlags: index === 0 ? 2 : 0,
      fatal: { reason: 3, threadName: "?" },
      type: 1,
    }),
  );
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementation(async (payload) => {
      const request = Request.decode(payload);
      return Response.encode(
        Response.create(
          request.getStatus
            ? { status: { capacity: 16, stored: 5 } }
            : request.listIncidents?.startIndex === 0
              ? {
                  incidentPage: { total: 5, startIndex: 0, incidents: records },
                }
              : { error: { message: "second page lost" } },
        ),
      ).finish();
    });
  const writeText = jest.fn().mockResolvedValue(undefined);
  Object.defineProperty(navigator, "clipboard", {
    configurable: true,
    value: { writeText },
  });
  try {
    render(
      <ZMKAppProvider
        value={createConnectedMockZMKApp({
          subsystems: [SUBSYSTEM_IDENTIFIER],
        })}
      >
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await screen.findByText(/second page lost/);
    expect(
      screen.getByText(/unavailable \(minimal capture\)/),
    ).toBeInTheDocument();
    fireEvent.click(
      screen.getByRole("button", { name: "Copy diagnostic report" }),
    );
    await screen.findByText("Report copied");
    const report = JSON.parse(writeText.mock.calls[0][0]);
    expect(report.retrieval).toEqual({
      complete: false,
      error: "second page lost",
      received: 4,
      expected: 5,
    });
    expect(report.incidents.map((i: Incident) => [i.id, i.uptimeS])).toEqual([
      [7, 10],
      [7, 20],
      [7, 30],
      [7, 40],
    ]);
    expect(report.incidents[0].captureFlags).toBe(2);
  } finally {
    rpc.mockRestore();
  }
});

it("ignores an old connection response after reconnecting", async () => {
  let oldAnswer!: (value: Uint8Array) => void;
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementationOnce(
      () =>
        new Promise((resolve) => {
          oldAnswer = resolve;
        }),
    )
    .mockImplementation(async (payload) =>
      Response.encode(
        Response.create(
          Request.decode(payload).getStatus
            ? {
                status: {
                  capacity: 16,
                  stored: 0,
                  buildTag: 0x12345678,
                  runtimeFlags: 1,
                },
              }
            : { incidentPage: { total: 0 } },
        ),
      ).finish(),
    );
  try {
    const first = createConnectedMockZMKApp({
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });
    const next = createConnectedMockZMKApp({
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });
    const { rerender } = render(
      <ZMKAppProvider value={first}>
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await waitFor(() => expect(rpc).toHaveBeenCalledTimes(1));
    rerender(
      <ZMKAppProvider value={next}>
        <IncidentsSection />
      </ZMKAppProvider>,
    );
    await screen.findByText("12345678");
    await act(async () =>
      oldAnswer(
        Response.encode(
          Response.create({
            status: { capacity: 16, stored: 10, buildTag: 0x87654321 },
          }),
        ).finish(),
      ),
    );
    await waitFor(() =>
      expect(screen.queryByText("87654321")).not.toBeInTheDocument(),
    );
    expect(screen.getByText("12345678")).toBeInTheDocument();
    expect(rpc).toHaveBeenCalledTimes(3);
  } finally {
    rpc.mockRestore();
  }
});

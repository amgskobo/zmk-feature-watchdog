import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";
import {
  IncidentsSection,
  SUBSYSTEM_IDENTIFIER,
} from "../../web/src/IncidentsSection";
import {
  Incident,
  Request,
  Response,
} from "../../web/src/proto/cormoran/watchdog/watchdog";

it.each([
  "success",
  "zero",
  "error",
  "wrong-kind",
  "wrong-count",
  "empty",
] as const)("individual delete feedback: %s", async (mode) => {
  const row = Incident.create({
    id: 100,
    source: 0,
    type: 2,
    bootOrdinal: 1,
    uptimeS: 10,
    reset: { causeBits: 4 },
  });
  let records = [row];
  let deletes = 0;
  const rpc = jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockImplementation(async (payload) => {
      const request = Request.decode(payload);
      let response: Response;
      if (request.getStatus)
        response = Response.create({
          status: { capacity: 16, stored: records.length },
        });
      else if (request.listIncidents)
        response = Response.create({
          incidentPage: {
            total: records.length,
            startIndex: 0,
            incidents: records,
          },
        });
      else {
        deletes++;
        if (mode === "success" || (mode === "zero" && deletes > 1)) {
          records = [];
          response = Response.create({ deleteResult: { deleted: 1 } });
        } else if (mode === "zero")
          response = Response.create({ deleteResult: { deleted: 0 } });
        else if (mode === "error")
          response = Response.create({ error: { message: "Backend failure" } });
        else if (mode === "wrong-count")
          response = Response.create({ deleteResult: { deleted: 2 } });
        else if (mode === "empty") response = Response.create({});
        else
          response = Response.create({
            status: { capacity: 16, stored: records.length },
          });
      }
      return Response.encode(response).finish();
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
    const button = await screen.findByRole("button", { name: "🗑️ Delete" });
    fireEvent.click(button);
    await waitFor(() => expect(deletes).toBe(1));
    await waitFor(() =>
      expect(screen.getByRole("button", { name: "🔄 Refresh" })).toBeEnabled(),
    );
    if (mode === "success")
      expect(
        screen.queryByRole("button", { name: "🗑️ Delete" }),
      ).not.toBeInTheDocument();
    else
      expect(screen.getByRole("button", { name: "🗑️ Delete" })).toBeEnabled();
    if (mode === "error")
      expect(screen.getByRole("alert")).toHaveTextContent("Backend failure");
    else if (mode !== "success")
      expect(screen.getAllByRole("alert")).not.toHaveLength(0);
    if (mode === "zero") {
      expect(screen.getByRole("alert")).toHaveTextContent(
        "No incident was deleted",
      );
      fireEvent.click(screen.getByRole("button", { name: "🗑️ Delete" }));
      await waitFor(() => expect(deletes).toBe(2));
      await waitFor(() =>
        expect(
          screen.queryByRole("button", { name: "🗑️ Delete" }),
        ).not.toBeInTheDocument(),
      );
      expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    }
  } finally {
    rpc.mockRestore();
  }
});

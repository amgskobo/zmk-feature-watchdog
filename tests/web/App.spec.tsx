import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { setupZMKMocks } from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import App from "../../web/src/App";
import { Response } from "../../web/src/proto/cormoran/watchdog/watchdog";
import { connectSerial, connectBluetooth } from "../../web/src/studioTransport";

// Mock the ZMK client
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("../../web/src/studioTransport", () => ({
  connectSerial: jest.fn(),
  connectBluetooth: jest.fn(),
}));

describe("App Component", () => {
  describe("Basic Rendering", () => {
    it("should render the application header", () => {
      render(<App />);

      expect(screen.getByText(/ZMK Watchdog/i)).toBeInTheDocument();
      expect(screen.getByText(/Firmware Incident Log/i)).toBeInTheDocument();
    });

    it("should render connection button when disconnected", () => {
      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();
    });

    it("offers Bluetooth for a BLE-selected keyboard and explains the route", () => {
      render(<App />);
      expect(
        screen.getByRole("button", { name: /Connect Bluetooth/i }),
      ).toBeInTheDocument();
      expect(screen.getByText(/selected keyboard output/i)).toBeInTheDocument();
    });

    it("should render footer", () => {
      render(<App />);

      expect(screen.getByText(/Watchdog Module/i)).toBeInTheDocument();
    });
  });

  describe("Connection Flow", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      jest.clearAllMocks();
      mocks = setupZMKMocks();
      (call_rpc as jest.Mock).mockResolvedValue(emptyStatusResponse());
    });

    it("should connect to device when connect button is clicked", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__watchdog"],
      });

      (connectSerial as jest.Mock).mockResolvedValue({
        ...mocks.mockTransport,
        close: jest.fn().mockResolvedValue(undefined),
      });

      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();

      const user = userEvent.setup();
      const connectButton = screen.getByText(/Connect Serial/i);
      await user.click(connectButton);

      await waitFor(() => {
        expect(
          screen.getByText(/Connected to: Test Keyboard/i),
        ).toBeInTheDocument();
      });

      expect(screen.getByText(/Disconnect/i)).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: "Status" }),
      ).toBeInTheDocument();
    });

    it("uses Bluetooth and releases its transport on explicit disconnect", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__watchdog"],
      });
      const close = jest.fn().mockResolvedValue(undefined);
      (connectBluetooth as jest.Mock).mockResolvedValue({
        ...mocks.mockTransport,
        close,
      });
      render(<App />);
      const user = userEvent.setup();
      await user.click(
        screen.getByRole("button", { name: "Connect Bluetooth" }),
      );
      await screen.findByText(/Connected to: Test Keyboard/);
      await user.click(screen.getByRole("button", { name: "Disconnect" }));
      await waitFor(() => expect(close).toHaveBeenCalledTimes(1));
      expect(
        screen.getByRole("button", { name: "Connect Bluetooth" }),
      ).toBeEnabled();
    });

    it("releases an opened transport when device-info discovery fails, preserving the error", async () => {
      mocks.mockFailedDeviceInfo();
      const close = jest.fn().mockResolvedValue(undefined);
      (connectSerial as jest.Mock).mockResolvedValue({
        ...mocks.mockTransport,
        close,
      });
      render(<App />);
      await userEvent
        .setup()
        .click(screen.getByRole("button", { name: /Connect Serial/ }));
      await waitFor(() => expect(close).toHaveBeenCalledTimes(1));
      expect(screen.getByRole("alert")).toHaveTextContent(
        "Failed to get device information",
      );
      expect(
        screen.getByRole("button", { name: /Connect Serial/ }),
      ).toBeEnabled();
    });
  });
});

function emptyStatusResponse() {
  return {
    custom: {
      call: {
        payload: Response.encode(
          Response.create({
            status: {
              capacity: 16,
              stored: 0,
              droppedSinceBoot: 0,
              recordingStopped: false,
            },
          }),
        ).finish(),
      },
    },
  };
}

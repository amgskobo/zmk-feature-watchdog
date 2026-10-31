import "./App.css";
import { useEffect, useRef, useState } from "react";
import { CLIENT_REVISION } from "./reportFormat";
import { ZMKConnection, useZMKApp } from "@cormoran/zmk-studio-react-hook";
import { connectSerial, connectBluetooth } from "./studioTransport";
import type { ManagedTransport } from "./studioTransport";
import { IncidentsSection, SUBSYSTEM_IDENTIFIER } from "./IncidentsSection";
import { getTelemetryUrl } from "./telemetryUrl";

export { SUBSYSTEM_IDENTIFIER };

function App() {
  const telemetryUrl = getTelemetryUrl();
  const app = useZMKApp();
  const transport = useRef<ManagedTransport | null>(null);
  const mounted = useRef(true);
  const [closing, setClosing] = useState(false);
  const [connectionError, setConnectionError] = useState<string | null>(null);
  const [cleanupFailed, setCleanupFailed] = useState(false);
  const disconnectRpc = app.disconnect;
  async function release() {
    disconnectRpc();
    const current = transport.current;
    if (!current) return;
    setClosing(true);
    try {
      await current.close();
      if (transport.current === current) transport.current = null;
    } catch (error) {
      if (mounted.current) {
        setCleanupFailed(true);
        setConnectionError(
          error instanceof Error
            ? error.message
            : "Connection cleanup failed. Close this tab before reconnecting."
        );
      }
    } finally {
      if (mounted.current) setClosing(false);
    }
  }
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      disconnectRpc();
      void transport.current?.close().catch(() => {});
    };
  }, [disconnectRpc]);
  useEffect(() => {
    if (!app.state.error || app.state.isLoading) return;
    const message = app.state.error;
    const current = transport.current;
    // Cleanup is an external asynchronous operation. Retain its outcome after
    // resetting the SDK's failed connection, rather than mirroring SDK state.
    void Promise.resolve()
      .then(async () => {
        if (!mounted.current) return;
        setConnectionError(message);
        setClosing(true);
        disconnectRpc();
        await current?.close();
        if (transport.current === current) transport.current = null;
      })
      .catch(() => {
        if (mounted.current) {
          setCleanupFailed(true);
          setConnectionError(
            "Connection cleanup failed. Close this tab before reconnecting."
          );
        }
      })
      .finally(() => {
        if (mounted.current) setClosing(false);
      });
  }, [app.state.error, app.state.isLoading, disconnectRpc]);
  async function connect(kind: "usb" | "ble") {
    setConnectionError(null);
    await app.connect(async () => {
      const current =
        kind === "usb"
          ? await connectSerial()
          : await connectBluetooth(() => {
              if (mounted.current) {
                disconnectRpc();
                setConnectionError(
                  "Bluetooth connection was lost. Reconnect to read again."
                );
                transport.current = null;
              }
            });
      if (!mounted.current) {
        await current.close();
        throw new Error("Connection cancelled.");
      }
      transport.current = current;
      return current;
    });
  }
  return (
    <div className="app">
      <header className="app-header">
        <h1>🐶 ZMK Watchdog</h1>
        <p>
          Firmware Incident Log · Fault / freeze / reset · UI {CLIENT_REVISION}
        </p>
      </header>

      <ZMKConnection
        zmkApp={app}
        renderDisconnected={({ isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            <p>
              Use the selected keyboard output: Bluetooth for BLE, or USB for
              USB. A visible USB port alone does not select USB.
            </p>
            <p>
              For USB, select the Studio port, not the console. Use the
              keyboard's Studio unlock key when requested.
            </p>
            <p>
              A split peripheral's USB is console-only. Read its incidents
              through central Studio and select the peripheral source after
              connecting.
              {telemetryUrl && (
                <>
                  {" "}
                  Use the configured{" "}
                  <a
                    href={telemetryUrl}
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    USB telemetry
                  </a>{" "}
                  tool for console logs.
                </>
              )}
            </p>
            {isLoading && <p>⏳ Connecting...</p>}
            {closing && <p>Closing the previous connection…</p>}
            {(connectionError || error) && (
              <div className="error-message">
                <p role="alert">🚨 {connectionError || error}</p>
                <p>
                  If device information cannot be read, check the selected
                  output and close other Studio clients.
                </p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                disabled={closing || cleanupFailed}
                onClick={() => void connect("usb")}
              >
                🔌 Connect Serial
              </button>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                disabled={closing || cleanupFailed}
                onClick={() => void connect("ble")}
              >
                Connect Bluetooth
              </button>
            )}
          </section>
        )}
        renderConnected={({ deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button
                className="btn btn-secondary"
                disabled={closing}
                onClick={() => void release()}
              >
                Disconnect
              </button>
            </section>

            <IncidentsSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>Watchdog Module</strong> - Firmware instability incident log
        </p>
      </footer>
    </div>
  );
}

export default App;

import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

// https://vite.dev/config/
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "VITE_");
  return {
    base: process.env.VITE_BASE ?? "/zmk-feature-watchdog/",
    plugins: [react()],
    define: {
      __WATCHDOG_TELEMETRY_URL__: JSON.stringify(
        env.VITE_WATCHDOG_TELEMETRY_URL ?? ""
      ),
    },
  };
});

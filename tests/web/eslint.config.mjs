import { fileURLToPath } from "node:url";
import config from "../../web/eslint.config.js";

// Apply the owner rules to the relocated test files as well.
const basePath = fileURLToPath(new URL(".", import.meta.url));
export default config.map((entry) => ({ ...entry, basePath }));

import { installConsolePrivacy } from "./consolePrivacy";

// Imported first by main.tsx, so the console is guarded before the app and
// the Studio SDK modules are evaluated.
installConsolePrivacy();

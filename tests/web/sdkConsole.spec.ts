import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import ts from "typescript";

// The console guard (web/src/consolePrivacy.ts) keeps a first string argument
// as a fixed label. This audit holds the pinned Studio SDK to that: every
// console call in its shipped JavaScript must be one reviewed here, with a
// string literal first. A dependency update that adds or changes a call fails
// this test for review; it no longer breaks the build.
const PACKAGES = [
  "node_modules/@cormoran/zmk-studio-react-hook/lib",
  "node_modules/@zmkfirmware/zmk-studio-ts-client/lib",
];

const REVIEWED = [
  'node_modules/@cormoran/zmk-studio-react-hook/lib/useZMKApp.js: console.error("Error reading notifications:", 1 more)',
  'node_modules/@cormoran/zmk-studio-react-hook/lib/useZMKApp.js: console.error("Connection failed:", 1 more)',
  'node_modules/@cormoran/zmk-studio-react-hook/lib/useZMKApp.js: console.error("Failed to get device info", 1 more)',
  'node_modules/@cormoran/zmk-studio-react-hook/lib/useZMKApp.js: console.error("Failed to get custom subsystems", 1 more)',
  'node_modules/@zmkfirmware/zmk-studio-ts-client/lib/index.js: console.log("Closed error", 1 more)',
];

function javascriptFiles(directory: string): string[] {
  return readdirSync(directory).flatMap((name) => {
    const path = join(directory, name);
    if (statSync(path).isDirectory()) {
      return javascriptFiles(path);
    }
    return path.endsWith(".js") ? [path] : [];
  });
}

function consoleCalls(path: string): string[] {
  const source = ts.createSourceFile(
    path,
    readFileSync(path, "utf8"),
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.JS,
  );
  const name = relative(".", path).replaceAll("\\", "/");
  const calls: string[] = [];
  // Comments and quoted text are not console calls; only executable code is.
  const visit = (node: ts.Node) => {
    if (ts.isIdentifier(node) && node.text === "console") {
      const member = node.parent;
      const call = member.parent;
      if (
        ts.isPropertyAccessExpression(member) &&
        member.expression === node &&
        ts.isCallExpression(call) &&
        call.expression === member
      ) {
        const [first, ...rest] = call.arguments;
        const label =
          first && ts.isStringLiteral(first)
            ? JSON.stringify(first.text)
            : "<not a string literal>";
        calls.push(
          `${name}: console.${member.name.text}(${label}, ${rest.length} more)`,
        );
      } else {
        calls.push(`${name}: unreviewed console use`);
      }
    }
    ts.forEachChild(node, visit);
  };
  visit(source);
  return calls;
}

it("finds only reviewed console calls in the pinned Studio SDK", () => {
  const calls = PACKAGES.flatMap(javascriptFiles).flatMap(consoleCalls);
  expect(calls.sort()).toEqual([...REVIEWED].sort());
});

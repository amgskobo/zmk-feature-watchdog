import {
  installConsolePrivacy,
  redactConsoleArgs,
} from "../../web/src/consolePrivacy";

describe("console privacy guard", () => {
  it("keeps a first label, numbers, booleans and error categories only", () => {
    const failure = new TypeError("private-device-setting-token");
    expect(
      redactConsoleArgs([
        "Connection failed:",
        failure,
        "private-name",
        42,
        true,
        null,
        undefined,
      ]),
    ).toEqual([
      "Connection failed:",
      "TypeError",
      "[redacted]",
      42,
      true,
      null,
      undefined,
    ]);
    expect(redactConsoleArgs([failure])).toEqual(["TypeError"]);
  });

  it("redacts plain object values, nested objects and class instances", () => {
    class Report {
      secret = "private-token";
    }
    const logged = redactConsoleArgs([
      "[watchdog] status",
      {
        collected: 3,
        allSources: false,
        key: "private-key",
        nested: { count: 1, value: "private-value", deeper: { count: 2 } },
        report: new Report(),
      },
    ]);
    expect(logged).toEqual([
      "[watchdog] status",
      {
        collected: 3,
        allSources: false,
        key: "[redacted]",
        nested: { count: 1, value: "[redacted]", deeper: "UnknownError" },
        report: "UnknownError",
      },
    ]);
    expect(JSON.stringify(logged)).not.toContain("private");
  });

  it("never lets a hostile object break or leak through the console", () => {
    const hostile = {
      get value(): string {
        throw new Error("private");
      },
    };
    expect(redactConsoleArgs(["label", hostile])).toEqual([
      "label",
      "UnknownError",
    ]);
  });

  it("guards every console method of the target it is installed on", () => {
    const calls: unknown[][] = [];
    const record = (...args: unknown[]) => calls.push(args);
    const target = {
      debug: record,
      log: record,
      info: record,
      warn: record,
      error: record,
    } as unknown as Console;
    installConsolePrivacy(target);
    const failure = new TypeError("private-device-setting-token");
    target.error("Connection failed:", failure);
    target.log("Closed error", failure);
    target.debug("private-first", "private-second");
    expect(calls).toEqual([
      ["Connection failed:", "TypeError"],
      ["Closed error", "TypeError"],
      ["private-first", "[redacted]"],
    ]);
  });
});

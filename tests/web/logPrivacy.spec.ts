import { errorKind } from "../../web/src/logPrivacy";

describe("console error privacy", () => {
  it("keeps fixed failure categories without serializing private data", () => {
    const failure = new TypeError("private-setting-name and value");
    Object.assign(failure, {
      cause: { token: "private-token" },
      code: "private-code",
    });
    expect(errorKind(failure)).toBe("TypeError");
    expect(
      errorKind(new DOMException("private-device-name", "NotAllowedError")),
    ).toBe("NotAllowedError");
  });

  it("rejects arbitrary names, primitives, and hostile formatting", () => {
    const stringify = jest.fn(() => {
      throw new Error("private");
    });
    expect(
      errorKind({
        name: "private-profile",
        message: "private",
        toString: stringify,
      }),
    ).toBe("UnknownError");
    for (const value of [null, undefined, "private", 42, { name: 123 }]) {
      expect(errorKind(value)).toBe("UnknownError");
    }
    expect(stringify).not.toHaveBeenCalled();
    expect(
      errorKind({
        get name() {
          throw new Error("private");
        },
      }),
    ).toBe("UnknownError");
  });
});

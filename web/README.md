# Watchdog diagnostic client

Common capture details show preserved CPU registers, assertion origins and
owner-defined metadata operations independently of incident type. Unknown
formats stay in copied raw reports. A partial flag or unavailable timestamp is
not a completed trace. Use the matching ELF to resolve addresses; metadata is
not an automatic replay of user input or a complete unwound stack.

This client reads the secured `cormoran__watchdog` custom Studio subsystem over
the Studio transport. The standalone app offers **Connect Bluetooth** and
**Connect Serial**. Choose the keyboard in the browser's device picker. Bluetooth
selection does not require the Studio UUID in advertisements; the selected device
must still expose the exact Studio service. No stored-device startup reconnection
or automatic pairing is performed. Match the transport to the keyboard's selected
output: BLE for Bluetooth, USB for serial. A mounted USB port alone does not activate
USB Studio. Select the Studio port rather than the console. Failed discovery and
explicit disconnect release the owned transport; an unconfirmed USB cleanup blocks
another attempt and asks you to close the tab. Use **Studio unlock** before reading; Messenger unlock
is a separate operation. The client does not erase any record merely by loading,
refreshing or copying a report.

Status shows capacity, stored count, dropped count, the running firmware tag,
monitor registration, software-only coverage, retained pending/save errors and
reset cause when supported. Previous-boot checkpoints are separate from stored
incidents and from the current firmware tag. Incident ordinals count recorded
incidents, not independent boots. Minimal capture explicitly marks its time as
unavailable. Old firmware remains readable without inventing missing diagnostics.

Refresh fetches status then ordered four-record pages. Interrupted/changed pages
remain incomplete, with received/expected counts and the error in the copied
report. Raw timestamps, duplicate legacy IDs and diagnostic/context/trace bytes
remain in received order. An old connection/source result cannot replace current
results. Clipboard errors are separate from retrieval errors.

Peripheral responses are subscribed before a request is sent and matched by
source plus request ID. RPC acknowledgement is bounded at 15 seconds; an
acknowledged relay response has a separate 3-second budget. Missing peripherals
can time out. These are client bounds, not physical split delivery guarantees.

```bash
npm ci
npm test -- --runInBand
npm run lint
npm run build
```

Use `npm run dev` for local development. Generated types follow
`../proto/cormoran/watchdog/watchdog.proto`; do not edit generated types by hand.
Tests, watch/coverage modes, development and builds generate these types before use.
The app uses the package-lock-resolved Studio hook/client. Tests exercise relay
ordering/abort/deadline cleanup, reconnect stale results, partial pages and copied
evidence with controlled transports. They do not establish physical BLE/USB.

Copy diagnostic report produces `zmk-watchdog-report-v1`. Decode addresses with
[the offline tool](../tools/decode_trace.py), after verifying the exact ELF against
its packaged manifest. A 32-bit firmware tag alone is not binary identity.
Unknown byte formats remain in JSON. The detector cannot reconstruct context that
an older firmware failed to capture. See [the module guide](../README.md) and
[the test boundaries](../tests/README.md).

## Current fault/reset interface

The UI opens with incident/status diagnostics and no mounted periodic health
reader. The source selector belongs to the fault/reset section. A split
peripheral's USB is console-only. Use central Studio and select a peripheral
source for that half's incident history.

The standalone app assumes no console service, local port or physical side.
An integrator can set `VITE_WATCHDOG_TELEMETRY_URL` to an explicit absolute
HTTP(S) tool URL before building. Empty or invalid values hide the links;
non-web schemes and embedded credentials are rejected. `web/.env.example`
documents the option. Private `.env` files stay outside Git. The URL is embedded
in the generated app, so review the built package before sharing it. Opening
the link performs no Studio or console operation and appends no device identity.

Performance/health views and automatic health polling have been removed.
The UI reads incident history and detector/runtime status only. USB telemetry
is independent and is not enabled by this application. Retired health field numbers
and names are reserved; the message types and UI entry are removed. Old health
requests receive the ordinary unsupported-request error before split dispatch.

## Console privacy

The local Vite plugin redacts the two locked Studio SDKs' exception console
arguments to fixed categories. New SDK console sites stop the build for review.
Transport/error propagation remains intact. Run `npm test -- --runInBand` and
`npm run build` with the existing locked dependencies; no install is implicit.
See [the owner tests](../tests/README.md) and [retirement map](../tests/retired-measurements.md).

## Dependency security

`package.json` overrides the Studio client's transitive `protobufjs` dependency
to `7.6.5`, including the patched UTF-8 helper. Use `npm ci` with this lockfile.
After changing dependencies, run `npm audit --omit=dev`, the Web tests and the
production build.

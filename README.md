# zmk-feature-watchdog

## Reviewed fork

This fork's reviewed source is on `codex/watchdog-review` in
[`amgskobo/zmk-feature-watchdog`](https://github.com/amgskobo/zmk-feature-watchdog/tree/codex/watchdog-review).
Select that repository URL and revision in the module's west project entry.
Keep the module's documented imports enabled where required. The original
upstream installation examples below describe the upstream repository.

The dependency manifests select the pinned Custom Settings revision
`c4071b095c3007d1deb055c609603ca9b5897d14` from its reviewed fork.
Those imports select that registry for ordinary owner tests and consumers.
A keyboard's explicit local override is a different source selection: record
its URL, full commit and working-source identity, and validate that combination
separately rather than treating the manifest pin as its resolved input.

The isolated test manifest selects DYA
`e5c9b6915b56801193e359dd9bad4a167ce0d1b8` from
`https://github.com/cormoran/zmk`. Keep that core unchanged and resolve its
complete imported graph. The native tests select an explicit owner input
fixture; they do not patch the upstream mock or require a local core commit.

Records workqueue freezes, fatal errors and selected reset causes. Each split
half owns its store. Optional custom Studio RPC reads/deletes local records or
requests a peripheral's records through the fork's split relay.

The common capture path is independent of the failing module. With
`CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT=y` (default with diagnostics), fatal and
freeze incidents also retain a versioned CPU/operation extension. On Cortex-M3/M4
this includes checked r4-r11 when the core supplies them, fault-address/status
registers and bounded code-address candidates. A software oops can lack its
original interrupt stack; missing evidence is marked partial rather than guessed.
The optional assertion adapter preserves the ordinary Zephyr assertion action
while recording its call site and source line. Custom assertions that bypass
that hook still enter ordinary fatal capture.

Any module can include `cormoran/zmk/watchdog_context.h` and call
`zmk_watchdog_operation(domain, event, arg0, arg1)` with numeric metadata before
and after an operation. The last four entries retain sequence, milliseconds,
call site and two full-width arguments. No periodic sampling is added. The BLE
adapter is one consumer; domains/events are not a controller-specific format.
Record metadata only, never input text, keys or peer identities. This is bounded
diagnostic history, not a complete execution recorder or automatic input replay.
Timed operation calls require a context where Zephyr uptime can be read safely;
the minimal fault path and assertion adapter do not call that timed recorder.

## Enable and inspect

Add this module to the west manifest of a ZMK fork supporting custom Studio RPC.
Enable `CONFIG_ZMK_WATCHDOG=y`; on the central enable `CONFIG_ZMK_STUDIO=y` and
`CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y`. Open the module UI, connect using Studio
Serial or Bluetooth in the standalone client, matching the selected keyboard output; perform
`&studio_unlock`, then Refresh. RPC is secured by default.
Messenger authentication remains a separate protocol. Fatal register words can
contain application data; the optional unsecured RPC exposes those diagnostics.

Defaults are 16 incident slots and a 10-second software freeze timeout, fed
twice per timeout. Low-priority queue monitoring defaults on; keyboards can
override it. Runtime status reports actual enabled/armed detectors, build tag,
reset cause, retained checkpoints and pending/persistence/validation failures.

Performance/health measurement has been retired: its collectors, enable option,
health history, polling UI, graphs and capture tools are removed. Freeze/fatal
capture, incidents, operation checkpoints, boot summaries and secured
incident/status RPC remain. Legacy GetHealth wire tags are reserved; such a
request is answered with the ordinary unsupported-request error, without
sampling or split requests. USB telemetry is
an independent module and is currently on hold; this module does not enable it.
See [the retirement map](tests/retired-measurements.md).

## Recovery boundaries

Monitoring arms during initialization after the queues exist. Feed work must
execute on its monitored queue; the software timeout can recover a queue stall
only while the timer interrupt can execute.

Fatal/freeze capture shares a non-blocking entry guard and fixed records.
Minimal evidence commits before optional clock/name/stack capture; nested faults
proceed directly to reboot. Minimal records explicitly show unavailable uptime.
Fault capture does not write Flash or wait for a lock. Software-only monitoring
cannot guarantee restart if CPU, interrupts or clock stop. `task_wdt_init(NULL)`
does not arm hardware, even when HW_FALLBACK is enabled elsewhere. The selected
Zephyr can still schedule its background software timer. IRQ priority alone
cannot exclude Watchdog as a contributor to radio timing.

Pending publication uses CRC and an atomic commit marker. Conversion waits for
settings readiness, serializes callers, preserves failed saves and retries with
one-second to one-minute backoff. Pending data clears after successful append or
the documented full-store drop. Reset between Flash append and RAM clear is not
an exactly-once transaction. Power loss/bootloader writes can erase retained RAM.

### Software-only Task WDT configuration

This module owns the `TASK_WDT_MIN_TIMEOUT` default while
`ZMK_WATCHDOG_FREEZE_DETECT=y`. It follows the software freeze deadline, capped
at Zephyr's supported 10000 ms maximum. A 10000 ms monitor therefore uses a
10000 ms minimum instead of Zephyr's generic 100 ms background interval;
queue-owned feeds still run every 5000 ms. For deadlines from 10 to 10000 ms,
the minimum matches that deadline; for longer deadlines through 600000 ms it
stays at 10000 ms, so a background wake every 10 s can remain. This is a timer
interval in milliseconds, not a hardware reset guarantee.

Keep Zephyr's normal `TASK_WDT_HW_FALLBACK=y` finite-timer path. The owner test
graph pins Zephyr `10ba6d0cb38bc3d258775d27982f707599320085`. That revision
initializes an empty channel set with `INT64_MAX` when fallback is off;
its absolute-timeout encoding overflows with 64-bit timeouts. Strict UBSan
reproduces this at `subsys/task_wdt/task_wdt.c:79`, including before any channel
can be added. The owner avoids that path without patching Zephyr or disabling
sanitizers. An unchanged `task_wdt_init(NULL)` supplies no hardware device.

Explicit global fallback and minimum settings still override defaults. Other
Task WDT clients must coordinate the global values and service initialization;
a shorter explicit minimum can restore background wakes. Explicit fallback n
is preserved as a configuration choice but is not an accepted runtime choice
on this selected core. Disabling this owner or its freeze detector preserves
Zephyr's ordinary minimum of 100 ms. No keyboard-specific workaround is needed.

No new thread, buffer, timer, persistence or runtime allocation is introduced.
The finite fallback path retains Zephyr's existing fallback state/code; inspect
the selected linked map for costs. With 10 s channels fed every 5 s, actual
Task WDT timer starts occur at initialization, channel arming and feeds, with
no 100 ms expiries. The owner tests this under strict ASan/UBSan for one and
two monitored queues. The original 100 ms configuration must fail its idle
assertion, and fallback off must reproduce the named core overflow, each with
three runs of its unchanged binary. Real board current, queue-stall recovery
and USB/BLE/split/reset boundaries still require matching installed images.

## History and diagnostics

Old v1/v2/v3 and current v4 records remain readable without rewriting or renumbering.
Older firmware cannot decode newly written v4 records; keep a matching reader
and firmware build when inspecting new captures after a downgrade.
`bootOrdinal` is a persisted incident ordinal, not a count of all boots. Full
stores drop/count new incidents without overwriting history or writing Flash;
recording resumes after explicit deletion. `CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST=y`
changes that policy: a full store replaces the incident with the lowest ordinal (the
oldest), so the newest `CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS` are kept and recording
never stops (`stopped=0`); an incident that happened sooner than
`CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S` (default 10) after boot, as in a boot loop,
is still dropped and counted. A positive minimum uptime gates replacement by
incident age; zero permits every incident to replace a record. This is not a
global Flash write-rate limit or wear guarantee. RAM reads never wait for locks held
across settings I/O. Corrupt types and bounded strings cannot become unsafe RPC
reads. Freeze stack addresses are candidates, not a complete unwind.

Recent diagnostics retain seven operation entries, cached state and a build tag;
BLE observers do not log keys or peer addresses. Previous-boot CRC checkpoints
remain separate from incidents and cannot identify a stopped PC. Copy before
another reset. Keep the matching ELF and manifest with full `elf_sha256`; a tag
alone is not image identity. Decode using `tools/decode_trace.py --help`.

Keep original incident exports, captures and device/image matching records
private. Raw incidents can contain application register words, thread names and
code locations. Before sharing a selected report, inspect its contents,
filenames and metadata for credentials, host/device identities and private
paths. Use synthetic labels in a sanitized copy while retaining the original
identity mapping, order, timestamps and retrieval errors privately. Preserve
public project/API identifiers, licenses and attribution.

Watchdog monitoring and capture are local to each half; no periodic split
sharing is added. Only explicit Studio status/list/delete requests use the relay.
The selected core API broadcasts, so the adapter accepts source 1 only when
exactly one peripheral is configured. Other sources and multiple-peer relay
requests fail before sending, preventing deletion on an unrelated peripheral
and cross-peer page mixing. Local Studio requests remain available. A transport
send failure returns an error; an accepted request with no answer times out.
Replies use source/request ID. Nonempty indexed pages/extensions must be complete, within the existing
128-byte event bound. Studio responses stream without enlarging TX buffers.
Historical rig checks do not establish current production acceptance.

The peripheral retains four pending relay requests in a FIFO and executes them
serially with a separate worker-owned input. Each invocation executes one
request and returns to its queue, letting queued freeze-feed work run before
the next invocation. A single request's synchronous transport wait is still
subject to the recovery boundaries above. A full FIFO returns a busy error
with the refused request's ID; accepted requests never replace each other's
bytes. Queue storage is four request structs plus kernel queue metadata, with
no new thread, heap or periodic work. An individual Delete verifies its result
and count and reports when no incident was deleted; refreshed history remains
available for a later retry.

Studio locking clears the client's incident/status display and retires pending
reads and deletions. Unlock starts a fresh retrieval; a previous response cannot
restore the retired view. Local replies and relay acknowledgements have a
15-second collection budget, configured explicitly in the pinned SDK. A deferred
peripheral answer has a separate three-second budget after acknowledgement.
Client cancellation retires results and listeners; it cannot retract an operation
already sent to firmware.

## Development

Read [the design](DESIGN.md), [test commands and limits](tests/README.md) and
[Web instructions](web/README.md). Run the owner native/store/Cortex-M/decoder
and Web checks specified there; build integration separately. Actual production
USB/BLE/split, Flash, power/reset and retained-RAM acceptance require board evidence.
Fault injection defaults off and belongs only in explicitly selected test builds.

The selected unchanged DYA core still has a signed state-bit shift in its
test-only `app/module/drivers/kscan/kscan_mock.c:103` path. Watchdog's five native
scenarios instead select `CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE` and a
distinct test device using public Kscan callbacks. Literal input/order and
completion checks supplement the original unchanged snapshots and C assertions.
Two fixtures deliver their configured input: `test` retains the actual core
keymap/HID Press A/release path, and `split_peripheral` retains callbacks and
its existing relay assertions without host HID. The original `watchdog`,
`studio` and `studio_unsecured` tests deliberately wedge sysworkq at APPLICATION
99 before the first 10 ms input callback, then exit before main. Their input
completion is NOT APPLICABLE; original freeze/checked-init/exit/snapshot
assertions and an observed zero-input/no-main terminal outcome remain mandatory.
Strict ASan/UBSan remains enabled with recovery disabled.
`CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE` supplies both runtimes to
native_sim's separate final host link; both native fixture flags stay off on
production boards. Separate named controls select the original unchanged core
mock and require its precise shift
diagnostic in three runs per binary. Those expected failures are not product
successes or a repair of the upstream driver. Keep final compiler flags and raw
failures with results; see [test limits](tests/README.md). GPIO/radio/browser and
physical acceptance, and the separate unsafe Task WDT fallback-off boundary,
remain independent.


### USB boot diagnostic summaries

With `CONFIG_ZMK_WATCHDOG_DIAGNOSTICS=y` and INFO logging enabled, the module
starts one bounded history pass 5s after boot. It waits at most 30 readiness
attempts, then reads at most the initially reported capacity/count, one record
per second. No incident or settings are cleared. Enumeration errors remain
visible; concurrent store edits can change which record occupies an index.

The log contains build/reset flags, count/capacity/drop state, incident IDs,
ordinals/times, fatal reason/PC/LR, freeze channel/state and bounded diagnostic
breadcrumbs. It excludes names, general register values, input values and peer
identifiers. `source=0` in these boot log lines means the prior checkpoint;
`source=N` refers to enumeration indexN-1 in this report, not the Studio split
source field. Use each record's captured build tag with its matching ELF.
A firmware upgrade's retained checkpoint may belong to an earlier image.

Peripheral relay stage IDs 30..34 mean execution begin, execution returned,
send begin, send returned and worker completion. They record sizes/kinds only.
An unmatched begin localizes the last published stage; it does not alone prove
a fault cause. USB capture may miss early output or disconnect during reset.

A fatal PC/LR is emitted only if it lies within the current image's executable
text. Zero means absent or rejected; a permitted address still needs the
record's matching build/ELF and is not proof of a valid exception frame. This
prevents corrupt frame words and addresses in settings/other memory from being
exported as code locations. General CPU register payloads remain excluded.


## Retired measurement interface

Performance/health dashboards, input capture and the health report reader are
removed. Historical results and exact old source remain in the external archive
listed in [the retirement map](tests/retired-measurements.md).

### Boot clock range

New incident capture derives uptime seconds from a uint32 millisecond boot
clock. Capture assumes a boot shorter than its full period (about 49.7 days).
Pending-save backoff uses unsigned elapsed milliseconds, including wrap at the
clock boundary. This changes neither record versions/layout nor loading of
older incident timestamps, and adds no periodic restart policy. Kernel/event
ABIs and stored record fields keep their existing widths.

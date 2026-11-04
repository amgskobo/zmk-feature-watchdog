# zmk-feature-watchdog

## Reviewed fork

This fork's reviewed source is on `codex/watchdog-review` in
[`amgskobo/zmk-feature-watchdog`](https://github.com/amgskobo/zmk-feature-watchdog/tree/codex/watchdog-review).
Select that repository URL and revision in the module's west project entry.
Keep the module's documented imports enabled where required. The original
upstream installation examples below describe the upstream repository.

The dependency manifests select the available Custom Settings revision
`c4071b095c3007d1deb055c609603ca9b5897d14` from its reviewed fork;
production imports and isolated tests use the same registry implementation.

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

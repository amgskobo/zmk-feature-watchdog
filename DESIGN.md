# Watchdog design

## Capture and monitoring

The store, capture paths and transport adapters have separate responsibilities.
Each split half owns its incident store. Capture publishes a retained-RAM record;
later boot work persists it. Studio RPC and split relay read that store without
making it depend on either transport.

Minimal and enriched common captures share one envelope initializer. Its
explicit timed/untimed mode keeps minimal publication free of clock access.
Operation entry size/count/offset and relay fragment size/count/completion mask
are named once; compile-time checks bind them to the record/protobuf capacity.
Relay FIFO depth derives from a maximal page, preserving the 21-event bound.
Offline decoding separates metadata/identity validation, candidate collection,
and external ELF lookup; CLI input/output and refusal rules remain unchanged.

Monitoring arms at APPLICATION initialization after the system and ZMK
low-priority workqueues exist, so a queue that has already stopped cannot
prevent it. Each monitored queue feeds its own task-watchdog channel twice per
timeout. Runtime status reports registration/scheduling failures.

Fatal and freeze capture share a nonblocking entry guard and fixed records.
They publish minimal reason or queue/channel evidence before optional clock,
name, register or stack collection. Nested capture reboots without replacing
the outer record. Capture performs no Flash write, allocation or module-lock
wait. The kernel fault-entry, clock/name access and SoC reboot paths remain
external boundaries; arbitrary corrupted kernel state is not made safe.

The software timer must still execute for freeze detection to recover a queue
stall. task_wdt_init(NULL) supplies no hardware watchdog. A CPU, interrupt or
clock-wide stop can prevent capture and restart. IRQ priority or successful
compilation cannot establish physical radio/reset behavior.

### Cortex-M trace

On supported single-core Cortex-M3/M4 configurations without userspace, capture
examines the monitored queue's thread, not whichever thread the timer interrupted.
It validates SRAM/thread bounds and scans at most 1024 bytes for six code-address
candidates. Saved PC/LR and BL/BLX-compatible words are candidates, not an unwind.
FP/alignment exclusions remain explicit; other architectures retain queue/time
detection without a stack scan. Fatal ESF reads check alignment and SRAM bounds.

The optional 36-byte FreezeDetail.trace is little-endian: format, thread state,
flags and address count occupy bytes 0..3; wait queue and SP occupy 4..11;
six u32 address slots occupy 12..35. Consumers must validate the format/count.

## Pending conversion and reset audit

Pending publication invalidates an atomic marker before updating content, then
publishes CRC and the marker after a completed record. CRC includes its sequence.
Boot conversion waits for settings readiness and serializes converters. A storage
failure retains the record and retries with bounded one-second to one-minute
backoff. Successful append or the documented full-store drop clears it; repeated
conversion cannot clear a newer published record or repeatedly count that drop.

A reset between successful Flash append and RAM clear can repeat an incident.
Power loss or bootloader writes can erase retained RAM. This is not an exactly-once
physical transaction. Reset audit preserves interesting cause bits on read/save
errors, retries clearing after persistence, and avoids duplicate append when
only clearing failed. Pending, invalid-pending and persistence/audit errors appear
in live runtime status, separately from incident history.

## Storage and compatibility

Version 4 records are 336 packed bytes; Flash slots add a four-byte ID header.
The first 144 bytes retain the v3 layout. The loader accepts exact v1/44,
v2/68, v3/148 and v4/340 version/slot-length pairs without
rewriting history. Unknown kinds, corrupt lengths and malformed keys reject
without cache mutation. The 80-byte diagnostic extension and fatal context are
zeroed on v1/v2 loads. The new 192-byte common extension is zero on v1-v3
loads, and an old reserved flag cannot claim that extension. Existing records
are neither rewritten nor renumbered. The store costs 192 additional RAM bytes
per configured slot; protocol buffers and capture state add fixed costs. Check
the selected firmware's linked RAM/Flash totals separately.

The incident ordinal increments per persisted incident, not per boot. Incident
and metadata writes are separate operations. Settings commit advances a stale
RAM ordinal beyond retained records using modulo-32-bit ordering before recording
resumes. IDs remain unique among retained records despite metadata failure or
uint16 wrap; deleted or evicted IDs can eventually be
reused. Legacy duplicate IDs remain enumerable but reject ambiguous by-ID access
or deletion. Full stores drop/count new incidents without overwriting history,
unless `CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST` is set: then an incident at or after
`CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S` of its boot replaces the stored incident with
the lowest persisted ordinal (compared modulo 2^32); the old record stays if the write
fails, IDs keep increasing, and an earlier incident is dropped as above. A positive
minimum uptime suppresses replacement by earlier crash-loop incidents in a full
store. Zero intentionally permits immediate replacement. This is an incident-age
gate, not a global Flash write-rate or wear guarantee; free slots and multiple
qualifying appends are separate cases.
Partial delete failure keeps failed records and successful deletions for retry.

Settings readiness comes from the module's h_commit. Writers do not hold the
RAM mutex across settings I/O. Append callbacks run outside module locks. Status
reads take one coherent RAM snapshot. Fixed record strings are bounded before RPC
encoding, including records whose name field has no terminating NUL.

Record flag bit 0 marks valid Cortex-M fatal context (r0-r3, r12, xPSR).
Bit 1 marks minimal/incomplete capture; its uptime is unavailable, not evidence
of a zero-second incident. Incident.capture_flags and RelayIncidentCore.capture_flags
use additive protobuf field 10. Existing field numbers stay unchanged and no
64-bit protobuf types are used. Bit 2 marks the additive common capture extension
in Incident field 11. This flag is independent of the fatal ESF-valid flag.

### Common CPU and operation evidence

The shared recorder does not depend on a BLE/controller-specific structure.
`ZMK_WATCHDOG_CAPTURE_CONTEXT` defaults on with diagnostics and adds the same
extension to fatal and freeze capture. Minimal publication snapshots completed
metadata operations before reading clocks, thread names or exception pointers.
Minimal capture has no capture-time capability; a zero timestamp is not an age.

Format 1 is exactly 192 little-endian bytes:

| Bytes | Meaning |
| --- | --- |
| 0..3 | Format 1, flags, code-address count <= 6, operation count <= 4 |
| 4..15 | Capture milliseconds, assertion call site and source line |
| 16..31 | Checked scan/target SP, MMFAR, BFAR and ICSR |
| 32..63 | r4-r11, only when the callee-valid flag is set |
| 64..87 | Six code-address candidate slots |
| 88..95 | Dropped metadata writes; capability bit 0 means capture time is available |
| 96..191 | Four 24-byte entries: sequence/time/site u32, domain/event u16, arg0/arg1 u32 |

Flags are partial/bounded (bit 0), callee-valid (1), fault-registers-present (2),
checked stack (3), assertion origin (4), unsupported CPU capture (5), and
interrupted ISR (6). MMFAR/BFAR contents are meaningful only with the matching
CFSR validity bits. Register payloads may contain application data and keep the
existing secured retrieval boundary. Code candidates are not an unwind.

On Cortex-M3/M4, EXTRA_EXCEPTION_INFO supplies saved r4-r11. Every pointer and
read length is checked against SRAM bounds. A copied software-oops ESF can omit
its original MSP and EXC_RETURN; capture does not infer an interrupt stack from
that copy. Unknown/extended FP frames are not scanned. A checked thread PSP can
scan at most 1024 stack bytes for up to six call-compatible code addresses, and freeze
capture retains the existing target-thread trace. Other architectures retain
metadata with the unsupported CPU flag. Wide native code pointers are reported
as unavailable rather than silently narrowed into plausible 32-bit addresses.

`zmk_watchdog_operation()` is a noinline public metadata API. Its call site,
numeric owner domain/event and two full-width values are recorded with sequence
and time; `_at` accepts an explicitly known call site for adapters. Domains are
owner-defined; domain 0 is reserved and domain 1 is the BLE
adapter. BLE arg0 packs handle16/role8/profile8, with 255 for unmatched profiles;
arg1 holds its event value. These are observations, not a peer identity or proof
that a particular link caused a later failure. Do not record text, key material
or peer addresses. Four entries are a bounded metadata window, not every
instruction or enough input to automatically replay arbitrary failures.

Writers try an atomic guard and publish an inactive buffer. Interrupted writers
are counted; readers retry at most twice and never wait for the interrupted
writer. Eviction remains visible across sequence wrap. The assertion adapter
wraps `assert_post_action` and forwards the original action exactly once. A
clock read before forwarding could deadlock an assertion inside clock code, so
the adapter publishes only its atomic origin; minimal capture retains it before
reading a clock or calling the ordinary metadata recorder. A
returning test action clears its origin, avoiding stale attribution. Assertions
that bypass this hook still reach the ordinary fatal handler; the wrapper does
not change their severity or claim that all assert macros share one hook.

## Operation diagnostics and build identity

Diagnostic format 1 occupies 80 bytes, with little-endian integer fields:

| Bytes | Meaning |
| --- | --- |
| 0..3 | Format 1, count up to seven, flags, observer capabilities |
| 4..7 | 32-bit firmware tag |
| 8..11 | Cached active profile, endpoint, USB state, observed BLE links |
| 12..15 | Dropped writer events |
| 16..23 | CFSR/HFSR, valid only with diagnostic flag bit 2 |
| 24..79 | Seven entries: u32 uptime_ms, u8 event, u8 profile, u16 value |

Diagnostic flags mark evicted history (bit 0), partial/inconsistent snapshot
(bit 1) and valid fault status (bit 2). Capability bit 0 marks the BLE adapter.
Unknown cached values/profile are 255; profile indices are zero-based. Event IDs
are defined in include/cormoran/zmk/watchdog_diagnostics.h. Return-error values
use two's-complement u16. Values have event-specific meanings. Uptime wraps after
about 49.7 days; preserve capture order instead of sorting timestamps.

Try-writers update an inactive RAM buffer and publish a complete checkpoint.
Fault readers never wait for an interrupted writer. Thread readers retry at most
twice, then report a partial snapshot. A PRE_KERNEL_2 hook validates retained
sequence/CRC/commit markers and copies the most recent completed previous-boot
checkpoint before starting fresh history. Interrupted writes retain the other
complete slot; invalid checkpoints are not presented as valid. SMP is gated off.

Previous checkpoints are boot evidence, not new incidents or a stopped CPU PC.
A normal pin reset can produce one too. Copy before another reset; retention
across power loss, reflash or arbitrary memory corruption is not guaranteed.
The BLE adapter records public operations/cached state without keys or peer
addresses. Linker wrappers forward the original call once with its parameters
and result. Calls inside the defining object can bypass GNU ld --wrap.
Fatal registers can contain application data; diagnostic RPC is secured by default.

### Live runtime flags

StatusResponse.runtime_flags uses stable bits defined in the diagnostic header:

| Bits | Meaning |
| --- | --- |
| 0..2 | Metadata supported, fatal enabled, freeze enabled |
| 3..5 | System armed, low-priority armed, registration/feed scheduling failure |
| 6..9 | Reset cause available, previous checkpoint available, corrupt checkpoint, software-only |
| 10..13 | Pending retained, pending invalid, persistence failed, reset audit failed |
| 14..15 | Common capture enabled, assertion call-site adapter enabled |

Armed reports successful channel/feed registration, not CPU-wide recovery.
Build configuration tags, source/config digests and core revisions are recorded
separately. A Zephyr or ZMK checkout that git cannot read records its revision as
`unknown` rather than failing the build. A tag is not a unique binary identifier: package the matching ELF and
verify its full SHA-256 before decoding addresses with tools/decode_trace.py.

## Retired performance measurement

The scheduler/input/BLE samplers, health history and corresponding configuration
and UI were removed on 2026-10-16. Freeze feeds only monitor queue progress.
The legacy health protobuf field numbers and names are `reserved`. An old
client's GetHealth request decodes as no request and gets the ordinary
unsupported-request error locally; it is never relayed and samples nothing.
Source/tests and historical acceptance gaps are retained externally; see
[the retirement map](tests/retired-measurements.md). USB telemetry is separate
and on hold. Fault/reset evidence and its limitations remain unchanged.

## Studio RPC, split relay and client

Incident notifications follow the same default Studio unlock requirement as
diagnostic requests. Locked incidents remain stored for later retrieval; they
are not queued for notification on unlock. Authorization is checked before
creating a notification and again before payload encoding after any transport
wait. The first encoder pass commits that decision for both nanopb sizing and
output. Revocation before that pass omits the payload successfully, preserving
the core's already-started frame. An authorized notification committed at sizing
finishes even if locking occurs before output; later notifications are stopped.
The explicit `ZMK_WATCHDOG_STUDIO_RPC_UNSECURED` option also applies to live
notifications; enabling it intentionally exposes diagnostic data while locked.
A relayed peripheral answer is notified through the same path, so an answer
that arrives after locking is not sent either.

The transport-independent executor handles local status, listing and deletion.
Studio responses stream through the existing TX ring buffer. Each half compiles
only its side of the relay: the central sends requests and accepts answers, a
peripheral accepts requests and sends answers. Monitoring/capture never polls
or synchronizes split stores. Only explicit Studio requests enter the relay.
The selected core exposes broadcast relay without a targeted send API; the
peripheral cannot determine its central-side slot. The adapter therefore accepts
source 1 only with exactly one configured peripheral, rejecting other sources
and multiple-peer requests before send. This prevents a targeted delete from
executing on unrelated peers and avoids interleaved pages in the single bounded
assembler. Local source 0 still executes locally in every configuration.
The existing wdq envelope is sent through the core API directly so encoding or
transport failures return an ErrorResponse instead of a deferred acknowledgement.
Nonzero correlation IDs use an atomic counter and skip zero on uint32 wrap.
Replies carry source/request ID. Indexed pages carry expected count
and diagnostic extensions separately from the record core. Common context uses
three ordered 64-byte fragments per incident, tied to page index, ID, source and
request. Missing, repeated, wrong-size or out-of-order required fragments reject
the page. A bounded 21-event FIFO handles a maximal four-record page burst
without raising the 128-byte payload bound. Full page/response storage remains
worker-owned static state rather than consuming its production-sized stack.
The assembler rejects incomplete pages, missing required extensions and mismatched
source/request/ID, while legacy ordered pages remain readable.

The client subscribes before sending a relay request and matches source plus
request ID. RPC acknowledgement has a 15-second deadline; acknowledged relay
response has a separate three-second deadline. Abort/deadline cleanup releases
listeners. Retrieval generations reject old connection/source/deletion results.
Partial reports retain received/expected counts and errors; copy preserves raw
bytes, duplicate legacy IDs and response order. Clipboard failure stays separate
from retrieval failure. Missing peripherals can time out; encoding tests cannot
establish physical split congestion or reconnect delivery.

Test injection defaults off. Another selected strong fatal handler conflicts at
link time; verify the final ELF's handler and disabled injection hooks. See
[module tests](tests/README.md) and [client checks](web/README.md) for executable
regressions and the boundary between software checks and board acceptance.

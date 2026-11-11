# Module-owned tests

Run from this checkout; no keyboard or simulator repository is required.

```bash
bash tests/run-host-docker.sh
ZMK_TEST_MODE=full bash tests/run-host-docker.sh
```

Quick runs the complete production `src/watchdog_store.c`, with its real public
header, under ASan/UBSan for capacities 16 and 1 in four policies (default; overwrite-oldest
with a guard of 10, 0 and 1 s) and, for the overwrite scenario alone, capacities 2 and 3. Full
additionally runs all of them optimized. `tests/store/mutate-overwrite.sh` holds the negative
controls of the overwrite policy (ten mutations, each must fail the fixture). Unknown modes fail. Assertions cannot be disabled with
NDEBUG. The CI host step runs all store scenarios; full adds optimized variants.

The mutation shell entry dispatches `tests/store/mutate_overwrite.py`. All ten
original mutation names and meanings remain. Every selector must match current
source exactly once, including multiline ordinal comparisons. Missing selectors,
compile failures, unclassified process failures and surviving mutations fail the
runner; compilation failure cannot count as a killed mutant. Each mutation still
runs capacities 16 and 1 against the unchanged complete store fixture. A killed
mutant requires an actual assertion or sanitizer diagnostic. Set
`WATCHDOG_MUTATION_ARTIFACTS=/absolute/new/directory` to retain exact mutated
source, compiler commands, raw ordered output, exits, binary hashes and results.
An existing output directory stops the run. The optional
`--compile-failure-negative-control` must be rejected at compilation, without
reporting a killed mutant.

The single [store fixture](store/store_test.c) covers:

- Ordered IDs, timestamps and payloads; reload, slot reuse, invalid arguments,
  capacity without overwriting, dropped count and callbacks outside the lock.
- Save failure/recovery; metadata-save failure preserves the saved incident.
- Overwrite policy (`CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST`): the oldest incident by persisted ordinal (modulo 2^32, not slot
  order) is replaced after slot and metadata writes, an incident earlier than the guard uptime is dropped and counted, a
  failed write keeps the old record, a replacement is not a drop, status and accessor never report stopped, reload keeps the order.
- Missing/stale metadata, consecutive failures and delete/reload recovery:
  retained IDs remain distinct and their exact payloads remain addressable.
- uint16 ID wrap through 65535/0/1; occupied IDs are skipped. Legacy duplicates
  remain enumerable and by-ID reads/deletes reject ambiguity without mutation.
- Single/multiple delete errors: every failed slot stays indexed and in flash,
  successful deletions remain deleted, first error is returned and retry works.
- Metadata and incident reads truncated at every prefix, invalid length/name/
  index, negative read results, valid loads and unchanged cache after rejection.
- Empty slot suffixes and malformed/out-of-range keys are rejected before any
  backend read or cache change; slot zero and the last valid slot still load.
- Independent v1 wire fixtures for freeze/fatal/reset preserve IDs, ordinals,
  times and details without writes. Every short read is rejected atomically.
  New traces survive append/save/reload; invalid version/length pairs cannot
  mutate the cache. Capacities 16 and 1 both exercise these cases.
- Unsupported record kinds reject append/load without writes or mutation;
  a coherent status snapshot preserves capacity/count/drop/full relationships.
- Persisted getters reject every short v4 read, missing/duplicate records,
  traversal/read failures, invalid lengths/types/versions and reused identities;
  all output bytes and returned IDs stay unchanged on error. A changed index
  returns EAGAIN, recovery retains every diagnostic/context byte, repeated
  retrieval reads storage and status/count do not. The history index and common
  read buffer fit `8 * capacity + 340` bytes without heap or getter stack copies.

Initial-load fixtures retain the raw successful settings bytes in their fake
backend. Later production getters read those bytes rather than inspecting an
internal full-record cache. Original ID, payload, legacy-prefix, rejection and
exact key-read assertions remain; invalid loads still leave the index unchanged.
The v4 fixture is retained explicitly while testing older format prefixes.

Mutex and settings backends are controlled substitutes. These checks establish
store behavior, not actual NVS writes, concurrent kernel scheduling, reboot,
retention, Studio transport or radio timing. Metadata-save failure can leave
persisted metadata stale; settings commit reconciles retained IDs and advances
the RAM ordinal beyond retained records before another append. Repeated failed
metadata writes and reloads preserve the newest records and exact payloads.
Deleted or evicted IDs can eventually be reused. The test does
not claim transactionality or physical power-failure durability.

For the settings/store lock boundary run the small real-kernel fixture:

```bash
bash tests/concurrency/run.sh
```

It compiles the complete production store/pending/reset units with real Zephyr
settings, mutexes and work scheduling, plus the resolved ZMK low-priority queue.
The backend and reset-cause driver are controlled substitutes. Twelve groups cover
settings readiness/reset ordering, load crossing append/delete/delete-all,
a stalled save with load and a second writer, failed pending save/backoff/recovery,
parallel conversion/newer capture preservation, invalid CRC/type and full-store
drop without repeated counting, and reset read/save/clear failure recovery without
duplicate append. Three additional groups pause real settings traversal/write
boundaries to exercise read versus delete, save versus read with available
RAM-only status, and two readers sharing one bounded read buffer with distinct
outputs. Each configured pause is consumed once; a later callback read cannot
reuse an earlier load's test barrier. Raw simulated timestamps and report order
are retained.
Each execution builds once and runs the identical binary three times. Set
`WATCHDOG_NATIVE_ARTIFACTS` to retain its configuration, binary and raw run logs.
The test also runs through `python3 -m unittest discover -v -s tests -p test_owner.py`; no radio or physical NVS/reset
acceptance is claimed.

For kernel/CRC/retention/RPC linkage, retain the existing owner native checks in
`tests/test_owner.py`, after resolving `west/west-test-workspace.yml` and reading actual
dependency instructions:

```bash
PYTHONPATH=tests python3 -m unittest -v test_owner.WestCommandsTests.test_zmk_test
```

For enabled features and board roles use the existing `test_zmk_build` command
in the documented west test workspace. Actual reset/flash/power and peripheral
delivery require matching production-board evidence. Fault injection remains
disabled in production.

## Cortex-M freeze trace boundary

```bash
python3 tests/trace/run.py --workspace /path/to/resolved/west-workspace \
  --artifacts /path/to/retained/trace-results
```

This builds the complete production trace unit on Zephyr's `mps2/an386`
Cortex-M4 QEMU target, using the selected workspace's Zephyr/CMSIS/CMSIS6 and
SDK. It builds once and runs the identical ELF three times. Actual kernel
threads wait for a mutex, sleep and execute floating-point work; a timer ISR
captures the monitored targets. Assertions cover wait pointers, saved/running
classification, FP/alignment exclusions, invalid stack bounds and bounded
address counts. The same ELF resolves each printed address using addr2line;
waiter/spinner caller names must match. Raw runs, config, ELF and decoded
addresses remain in the chosen artifact directory. This is a focused kernel
check, not a XIAO model, BLE test or reset-retention acceptance.

The existing native RPC checks encode/decode the optional trace using real
nanopb, retain trace-free records, and verify six-address split chunks within
the generated payload bound. No snapshots are auto-accepted. Web tests cover
wire decoding, malformed formats, legacy messages and diagnostic-copy contents.
Run the offline decoder's host checks with:

```bash
python3 -m unittest discover -s tests/decoder -v
```

The Studio fixture also uses the real store, core lock state, event manager and
nanopb notification callback to check locked append without notification,
unlocked encoding, and locking between event selection and payload encoding.
It sizes, encodes and decodes the complete RPC envelope. Locking before the first
encoder pass omits the payload without breaking that envelope. Locking after
the first sizing pass finishes the committed payload with a matching length;
nanopb's subsequent passes must not change the authorization decision.
Records remain stored in all cases, and unlock does not replay notifications.
The same assertions support `CONFIG_ZMK_WATCHDOG_STUDIO_RPC_UNSECURED=y`:
this explicitly opted-in build encodes notifications in both lock states.
`tests/studio_unsecured/` selects that flag while reusing the canonical Studio
keymap and complete C assertions. Its registry snapshot requires Unsecured;
the unchanged default fixture still requires Secured. The owner dispatcher
requires all completion markers with `unsecured=0` and `1`.
The unittest requires the RPC completion markers in the raw fixture log in
addition to the key snapshot; an early SYS_INIT test failure cannot pass merely
because later key input still matches. These are encoding/authorization checks,
not physical transport cancellation or radio acceptance.
The fixture's complete-message capture uses nanopb's generated maximum wire
size rather than the firmware TX ring size. The ring streams a larger message
in pieces; its configured size remains unchanged. Pagination assertions now
complete for records containing trace, CPU context and diagnostic extensions.

## Recent-operation / fatal diagnostics

The same documented owner commands exercise the new record format and RPC.
Store fixtures load v1, v2, v3 and v4 with exact version/length pairs, no rewrite,
and zeroed legacy extensions. Native streaming RPC includes all 80 diagnostic
bytes and all six fatal words. Native split fixtures encode/decode extensions
and run the production page assembler for duplicate legacy IDs, duplicate
payloads, wrong source/request/ID, out-of-order extensions, oversized input,
missing core/extension and legacy ordered pages. A failed page must be an
error, never a complete diagnostic result. No snapshot auto-accept.

`tests/trace/run.py` also exercises an interrupted diagnostic writer, nested
writer drop, bounded history eviction, NULL/synthetic Cortex-M ESFs and a real
`k_oops()` from a timer ISR. The production fatal handler captures xPSR and
breadcrumbs. The same ELF is run three times and fatal PC/LR are symbolized.
QEMU checks kernel/CPU capture only; do not infer nRF radio/reset fidelity.
The collector waits for a complete newline-terminated RESULT record before
terminating QEMU. Prefixes, delayed suffixes, EOF without a newline and timeout
cannot count as completion. `tests/test_qemu_capture.py` exercises these cases
through the canonical owner entry. Trace validation retains all original
kernel/ELF assertions and records all three trials, process exits and ELF hashes,
including validation failures. Raw output remains in received order.
The decoder and Web tests verify byte formats and chronological uptime wrap,
signed errno, fatal PC/LR, full-ELF hash rejection and per-incident tag mismatch.

## Powered reset diagnostic checks

The existing Cortex-M fixture also checks the production checkpoint code with
the same RAM and repeated startup hook calls: newest completed data, independent
boot histories, interrupted commit fallback, one/two invalid CRCs, sequence wrap
and detector registration flags. Its identical binary runs three times. These
are kernel/CRC/startup checks; they do not establish real nRF/bootloader retention.
Native RPC tests encode a maximal status with all 80 checkpoint bytes, full reset
bits and 32-bit dropped count through the existing split envelope. Public
capacity/count are uint16_t, so nanopb uses their real bounds; the complete relay
event must still fit 128 bytes. Web tests distinguish running/prior build tags,
retain raw timestamps/order and copy checkpoints without inventing an incident.

## Arming before a queue stop

```bash
bash tests/arming/run.sh
```

The complete production freeze/trace/capture units and resolved ZMK queue run
under the actual Zephyr native kernel. A SYS_INIT hook blocks sysworkq before
Watchdog initialization. Arming must still register its 100ms timer and
detect the stalled queue. Assertions cover timer ISR entry,
minimal then full publication, captured queue/channel and shared capture guard.
The exact binary runs three times; WATCHDOG_NATIVE_ARTIFACTS retains its inputs
and logs. Controlled pending/store/reboot stubs allow observation of the callback;
this is not an actual XIAO reboot test.

The Cortex-M fixture additionally covers invalid ESF pointer rejection, minimal
publication before enrichment, nested fatal entry without overwriting the outer
record, and guard release only when the test reboot hook returns. Native RPC
checks include non-NUL fixed-size record names and minimal-capture flag encoding.
These do not establish safety under arbitrary corrupted kernel state or recovery
from a CPU/interrupt/clock-wide stop.

## Current test ownership and compatibility

### Common context and reproducible fault evidence

The canonical owner entry now also runs `tests/context/run.py` and both ordinary
oops and assertion variants of `tests/trace/run.py`. The common native fixture
retains full-width arguments, domain/event, capture order, bounded eviction,
minimal time availability, interrupted/nested writer behavior and assertion
forwarding with and without file information. Each binary runs three times.
A compiled missing-publication control uses the unchanged assertions and must
exit 99 with an actual assertion diagnostic in all three repeats; compilation
failures or timeouts do not count as rejected controls. A second compiled control
restores a clock-taking metadata call before assertion forwarding; the unchanged
clock guard must reject it three times. Minimal capture and assertion forwarding
must succeed with clock access forbidden, including the assertion's origin.

The Cortex-M fixture preserves all previous kernel/FP/CRC/nested-fault assertions
and adds checked callee-register reads, invalid extended ESF/callee pointers,
the shared freeze envelope and generic asynchronous callback metadata. A second
binary triggers a real Zephyr assertion in the timer ISR, retaining its source
line and resolving the original call site against that same ELF. These are
deterministic generic capture fixtures, not a reproduction of a physical BLE race.

The real-store/RPC fixtures also retain all 192 common context bytes. Native page
assembly and the production peripheral relay test exercise three ordered 64-byte
fragments, maximal four-record/21-event pages and exact bytes. Eight required
negative controls reject missing, wrong-ID/index/source/request, wrong-size,
out-of-order and duplicated context fragments. The existing seven relay and
three reporter groups remain. The 2 KiB Cortex-M queue still requires 128 sampled
unused bytes with the 344-byte downstream stack reservation.

Offline and Web decoders validate formats/counts/capabilities, preserve raw
unknown data and original event order, and retain 32-bit unsigned values. Copy
includes the raw extension. Old reports remain readable. Run the full firmware,
store, decoder, Web/lint/build and consumer build matrices before delivery;
physical reset/retention, transport, radio and recurrence acceptance remain separate.

`python3 -m unittest discover -v -s tests/decoder` also checks damaged optional
common captures. The offline output retains `raw_incident` and
`raw_capture_context` before parsing. Unknown formats, invalid lengths/counts,
capabilities, missing required bytes and non-uint32 capture flags report a
`capture_context_error` (also in the existing `error` field) while preserving
valid primary Fatal registers/PC/LR or freeze traces and subsequent records.
The common extension is left unavailable rather than interpreted after failure.
Build-tag/ELF mismatches still prevent symbol lookup. Regression tests retain
the original failure, raw values, valid extended-symbol cases and the mismatch
negative control; accepting optional-data errors must not weaken identity checks.
Malformed Fatal register extensions and freeze traces likewise expose a
`fatal_context_error` or `trace_error` without discarding independent PC/LR or
common-capture addresses. Non-object incident/detail values are retained as raw
per-record errors, including falsey values, and later records still decode.
Invalid top-level report, incidents-array and manifest shapes are CLI usage
errors instead of Python tracebacks. Tests retain the original failures and
combine malformed details with the build-tag refusal control.
For actual ELF/addr2line acceptance, select `WATCHDOG_DECODER_ELF` and
`WATCHDOG_DECODER_MANIFEST`, put `arm-zephyr-eabi-nm` and
`arm-zephyr-eabi-addr2line` on the container PATH, and run the same decoder entry.
`tests/decoder/test_cli_recovery.py` resolves a real Watchdog diagnostic symbol,
exercises malformed fields/records and shape errors, and refuses a wrong tag.
Without both selected inputs this optional real-ELF class is explicitly skipped;
selecting only one is an error. `WATCHDOG_DECODER_CLI_OUTPUT` optionally retains
the synthetic reports, actual commands, exits and raw CLI output in a fresh
directory. Assertions and fixtures remain in the owner repository.
The identity suite also binds the peripheral's common-relay assertion header.
A configuration-only probe evaluates the actual owner CMake identity path,
changes only that header, and verifies automatic regeneration and a changed
source digest. It does not replace the real DYA firmware/Kconfig matrix.

### Central relay dispatch boundary

`python3 tests/relay-central/run.py --workspace RESOLVED_WORKSPACE --output FRESH_OUTPUT`
also runs through `test_owner.WestCommandsTests.test_relay_central`. It compiles
the complete production central unit, real nanopb, DYA event macros/manager and
Zephyr native kernel. Configured peer capacity and transport acceptance are
controlled boundaries. Capacity 1 and 2 variants each run the identical binary
three times. Assertions preserve wdq envelope/request bytes, source selection,
nonzero IDs and uint32 wrap, status/list/delete dispatch, zero sends on rejected
sources/configurations, explicit ENODEV/EIO errors and 128 sampled unused bytes
on the 2 KiB main stack. The unchanged assertions reject the original production
central unit; compile failures do not qualify as rejected controls. Real board
configuration/role selection remains in the full build matrix; this fixture does
not establish physical split/radio/transport or worst-case stack acceptance.

The single-peer variant also retains an incomplete four-incident page while a
non-list notification is blocked. All twenty remaining page datagrams must fit
the unchanged21-entry FIFO, and the notification payload and its mutex remain
owned through synchronous sizing/output encoding. Exact source/request IDs,
raw times, all diagnostic/context bytes and ordered status/page delivery are
required. Access denial, transport error and a subsequent successful reply must
release ownership without leaking the previous response.

The central worker builds both page and non-list replies in the existing
notification-owned response, keeping the independent page accumulator. It does
not allocate another full-page scratch response or move it to a stack/heap.
`tests/static_memory.py` accounts the actual central input object's linked RAM
sections, including kernel objects and the FIFO, from the additional
`module_watchdog_split_central_memory` ARM board map. It selects the production
relay with `CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=n` and requires the actual
owner C compilation and absence of the boot-scenario symbols. All ten original
functional/test board variants remain selected. In particular, the original
central scenario retains its two synthetic response buffers and init work;
their2960 bytes are real linked test storage and are not subtracted from a map.
The production variant's selected ABI budget remains5760 bytes; the original
two-scratch-response production layout uses8704 bytes and must fail the named
static-memory check. Parser controls reject missing owner/region data,
duplicates and out-of-region sections. This measures linked static storage,
not runtime heap or stack high-water use.

### Peripheral diagnostic stack boundary

Refactoring keeps the independent existing common-capture, 192-byte wire,
three-fragment/21-event relay and decoder assertions. The shared capture
initializer must still pass clock-forbidden minimal/assertion controls. Wire
sizes/counts are checked against fixed fixtures rather than derived from the
implementation constants; pure decoder extraction keeps all previous CLI,
raw-data, malformed-field and ELF/build refusal scenarios.

```bash
python3 tests/relay-stack/run.py --zephyr /resolved/zephyr --cmsis /resolved/cmsis \
  --nanopb /resolved/nanopb --zmk /resolved/dya-zmk --artifacts /new/relay-stack-results
```

Also dispatched by `test_owner.WestCommandsTests.test_relay_stack`. Complete
production peripheral relay and request executor run with real nanopb and the
Zephyr Cortex-M4 kernel on a 2,048-byte workqueue. The selected unchanged DYA
checkout supplies its actual event macros, complete event manager and linker
fragment. A noinline live 344-byte downstream stack reservation represents the
declared 192-byte transport and 152-byte sender frames, followed
by a real kernel message queue. This is a declared stack-budget model; it does
not execute BT/radio or establish worst-case stack use. Store remains controlled.

Empty/populated maximal 80-byte prior status, empty/full/second/out-of-range
pages retain response IDs, record/extension order, counts and payloads. A seventh
group holds the first response in transport while another thread submits a
second request to the same work item: ordered IDs 7/8, status and checkpoint
bytes must remain independent. Seven relay and three reporter groups run from
the same ELF three times; ordered logs and source/dependency identities persist.
The fixture uses a 16 KiB deferred log buffer to retain the complete relay and
request-admission burst for the unchanged ordered-log assertions.
At least 128 sampled unused bytes reserve Cortex-M FP exception frames/context.

The former FIFO-only event substitute omitted real event/send stack cost.
The current fixture uses worker-owned static response storage and retains the
existing response/order/assertions with real event dispatch. Before-repair
sources and raw rejection records remain in private review evidence. No physical
radio or repair acceptance follows from this bound. Compilation uses temporary
Linux storage, retaining reviewable inputs, ELF/configuration and ordered output.

### Safe boot history and relay-stage observations

The same `tests/relay-stack/run.py` entry now enables the diagnostic getters
and exercises a maximal 80-byte prior checkpoint in status responses. It also
runs the complete production boot-report unit on the real 2KiB workqueue:
settings-not-ready 30-attempt limit, initial 5s scheduling, one-record-per-second
scheduling, empty store, all incident kinds, legacy records, unavailable formats,
bounded seven-entry diagnostics, enumeration failure, no record mutation, and
exclusion of names/general register payloads. Repeats retain the same ELF and
ordered output; assertions verify units after converting kernel ticks to ms.
The former raw-tick/ms assertion error is retained in the follow-up artifacts;
it was an assertion defect, not firmware timing nondeterminism.

`--mute-info-negative-control` builds with the production module log threshold
set to 0. The log contract must fail in all three identical runs, even if the C
scenario marker passes. No production source is rewritten and normal production
INFO remains enabled. Test transport still substitutes real event/BLE delivery.
Do not interpret the stack fixture or diagnostic logs as physical repair acceptance.

### Retired performance measurements

The performance/health collectors, UI and their feature-specific fixtures were
retired on 2026-10-16. Run `python3 -m unittest discover -s tests -p test_retirement.py -v`
for source/selection guards. The relay-stack fixture sends a legacy GetHealth
request as raw wire bytes and requires the ordinary unsupported-request error
without a sample store, retaining all seven incident relay
and three boot-report groups. The [retirement map](retired-measurements.md)
identifies archived unique assertions and physical gaps; they are RETIRED, not PASS.

This owner supports DYA custom Studio RPC. Stock ZMK is NOT APPLICABLE;
no feature-disabled stock check is required or counted as compatibility.

Canonical entries (run from the repository root):

```bash
python3 -m unittest discover -v -s tests -p test_owner.py
bash tests/run-web.sh
```

For a bounded complete firmware run, set `ZMK_TEST_MODE=full`, `J=1` and
`CMAKE_BUILD_PARALLEL_LEVEL=1` with a fresh `OWNER_TEST_OUTPUT`. The owner board
dispatcher already uses `-P1`; `J` separately bounds the supported core's native
fixture dispatch. CPU quota alone does not bound that dispatch.

[maintenance.json](maintenance.json) records supported cores, current commands
and explicit gaps. It is a reference map, not a passing test or a claim that
registration/coverage totals establish completeness. Preserve ordered outputs,
raw timestamps and the first failure. Read actual dependencies before execution.

`OWNER_TEST_OUTPUT=/absolute/new/directory` retains owner native/build outputs.
An existing directory fails before tests. Without it a fresh temporary directory
is printed and retained. No shared build directory is deleted. Use
`PYTHONPATH=tests python3 -m unittest -v test_owner.WestCommandsTests.<method>`
for a focused case; discovery runs the complete documented class.

Web assertions and Jest configuration live in `tests/web/`; application source
and installed dependencies remain in `web/`. The package scripts only dispatch
the owner configuration. No dependencies are installed implicitly by the tests.

Guarded production includes in `tests/support/` contain test-only scenarios.
Small production probes remain beside the state they expose. Public sample
settings and diagnostic/fault-injection APIs are production contracts, not
duplicated test algorithms.

The native verifier runs each built binary three times in independent empty
initial-flash directories, retaining raw output before filtering. It checks real
process exits, fatal assertions and every linked checked-init result before
comparing unchanged owner snapshots. A SYS_INIT error cannot be hidden by a
later matching key trace. Native scheduling and controlled APIs still do not
establish physical transport/retention. Test-only core/tool dependencies are
pinned; update pins only after reviewing the new sources and rerunning this
owner matrix. No network updates are implicit in the owner test command.

## Reproducible isolated checks

Use this owner's `west/west-test-isolated.yml` and its pinned test dependency
manifest. Test ZMK is unchanged `e5c9b6915b56801193e359dd9bad4a167ce0d1b8`;
Zephyr is `10ba6d0cb38bc3d258775d27982f707599320085`. Resolve the
manifest and verify actual checkout identities before running. Do not inherit
global CMake module overrides or moving production dependency references into
these isolated fixtures. The CI SDK image is pinned by digest in
`.github/workflows/zmk-module.yml`; production dependency manifests are separate.

Prepared workspaces must contain that exact upstream commit and the complete
Git-backed import graph. Source availability is a separate check from local
tests. Do not patch the core, substitute a local core commit or change an
authoring checkout to clear a fixture failure. Byte-verified isolated runtime
inputs and their metadata are separate from authoring checkouts.

Use a new `OWNER_TEST_OUTPUT` directory for every complete owner run. Native
repeats reuse one compiled binary with independent initial-flash state; keep
the first failure and raw combined stdout/stderr order. DYA-only compatibility
still means no stock-ZMK test requirement.

Keep Git metadata for projects that supply imported manifests or west commands,
including the pinned `zmk-west-commands` project. A retained directory of working
files alone cannot resolve a Git-backed west import. Check the complete selected
graph before execution; an import/setup failure is BLOCKED, not a passing owner
run. A separately verified core workspace may support focused entries, but does
not replace the complete owner workspace or its required matrix.

The test/tool static CI job installs pre-commit 4.7.0 and uses the pinned
Ruff/clang-format/formatting hooks. Its SKIP list avoids repeating the firmware
and Web jobs. Local full pre-commit execution may run those functional hooks;
prepare Web dependencies with the normal package lifecycle first where present.
A successful hook exit must also leave source inputs unchanged. Snapshot and
event-pattern bytes are excluded from whitespace rewriting.

For a proposed commit, pass every scoped modified and untracked file to the
unchanged configured hooks. Perform any private-index preparation in an isolated
check environment; leave the author's real index and Git identity unchanged.
Use the complete selected west workspace for the module-test hook and retain
its actual nonempty fixture completion. File-filtered hooks for unchanged Web
paths are not applicable; setup errors or failed applicable hooks remain open.


Physical retrieval records are private and are not distributed with this test
guide. No current physical acceptance is established by the public fixtures.
Keep radio/congestion/power/long-run and worst-case stack gates separate.


### Remaining client contracts

The Web suites retain connection cleanup/route choices, secured incident/status
reads, split request ordering/cancellation, trace/diagnostic decoding, privacy
and raw fault evidence export. The incident view additionally rejects a retired
measurement UI entry or background health request. Controlled transports do not
establish actual USB/BLE or physical input acceptance.

The cache helper below is a separate compatibility lane for the historical
Custom Settings revision `c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f`.
The ordinary production and isolated owner manifests select the current reviewed
fork revision; results from this older archive do not establish that selection.
Before using `PIN.tar`, require SHA-256
`3e815475a9756ef1b69d5352bc5d0284fec2b4cf20e1c402a474978010e83ff1`.
Keep the archive identity distinct from the current dependency pin.

For a prepopulated read-only cache outside an isolated west workspace,
`tests/prepare-workspace.py --cache CACHE --commands COMMANDS --settings-tar PIN.tar
--owner OWNER --output NEW_WORKSPACE` prepares exact temporary compiler inputs.
COMMANDS must be the documented clean 536260056f362edb2092905f2a75317bc8cb9bc2 checkout;
PIN.tar is a local `git archive c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f` export of
custom-settings. No fetching or dependency patching occurs. ZMK/Zephyr and owner
are copied into the temporary workspace because CMake resolves symlinks and the
pinned command framework requires cwd inside west. All owner-copy bytes are
verified against the owner; headers/modules keep their resolved pins. Run the
canonical owner command in NEW_WORKSPACE/owner with ZEPHYR_BASE and Zephyr_DIR
pointing to NEW_WORKSPACE/zephyr and its share/zephyr-package/cmake directory.
Keep raw failures; a production-manifest escape is a setup failure, not a pass.
The non-Cortex-M boot reporter omits uint32 code addresses: native host linkers
lack the Cortex-M text-region symbols. All detectors/history features remain
compiled. Native fixtures failed linking before this guard; the Cortex-M report
fixture retains its existing executable-address/privacy assertions.

## 32-bit timeout arithmetic

Variable millisecond reservations use a private owner header and preserve the
full uint32 input range, exact ceiling and configured kernel tick ABI. Fractional
products and divisions use uint32; only the required tick result is widened.
Zero/forever handling, callback ownership, cancellation and output policy remain
at their existing call sites. This introduces no new limit on timer settings.

`test_timeout_width.py` runs in the canonical host/owner entry. It compares every
0..65535 interval, full-width boundaries and 65536 deterministic full-width
samples at 14 rates against an independent wide oracle, for both tick forms.
Full retains optimized, ASan/UBSan, measured nonzero complete header line/branch
coverage and m32 object/numeric execution. Four identified arithmetic faults
must produce three qualified assertion failures per tick form. A numerically
correct wide-divider control must fail the object-width guard. Existing output,
state, timing and supported-core assertions remain enabled. Kernel/physical
acceptance remains separate from this numerical and object-code check.

Pending retry spans use unsigned elapsed milliseconds, with explicit zero-delay,
47-day, equality and wrap checks in the same fixture. Two additional clock
faults must fail qualified assertions three times per tick form. Capture uptime
seconds now comes from the uint32 boot-millisecond clock; supported capture
uptime is below one full clock period (about 49.7 days). Incident formats and
historical stored values are unchanged; no new restart policy is introduced.

The owner command boundary limits `zmk-build` to one target at a time by
default. `OWNER_TEST_BUILD_PARALLELISM` selects an explicit positive target
count and forwards the same budget to the common-context runner's existing
`--jobs` argument. The parser validates it before any context subprocess
starts. Account for other compiler jobs and concurrent owners in the global
budget. `tests/test_build_budget.py` exercises actual board/context dispatch
arguments with the default, explicit one/two and invalid budgets; invalid
values launch no child process. The original context entry, which omitted
`--jobs` and used the runner's two-job default, fails these context controls.
The canonical owner entry includes them without dropping any kernel variant,
assertion or board scenario.

Native result admission requires every `native_sim.keymap` fixture to have a
matching binary. `required_hooks.json` names the expected checked initialization
hooks and required feature/role configuration. Fixture-bound declarations also
reject missing configuration when both the request and build omit it; excluded
roles cannot replace the requested role. The verifier retains all three raw
runs before comparing unchanged ordered snapshots and rejects logged failures,
fatal output and missing/nonzero completions. `tests/test_native_verifier.py`
owns fourteen real-ELF admission controls, including a deliberately missing
native scenario, wrong-role and unrelated-fixture controls. Recoverable ASan
and UBSan programs independently prove a real diagnostic with exit 0 before
requiring rejection; the original verifier incorrectly admitted both programs
through all three repeats with matching checked initialization and snapshots.
Ordinary sanitizer configuration names and synthetic application markers remain
accepted. Retained raw diagnostics take precedence over successful process,
initialization and snapshot results.

The selected unchanged DYA core retains its test-only mock's signed `1 << 31`
fault. The five owner native scenarios select a distinct
`amgskobo,watchdog-test-kscan` device and
`CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE`, which depends on native_sim and
that compatible. The companion `CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE`
depends on native_sim and ASan and supplies the declared strict runtimes to the
public `native_simulator` target's separate final host link. Application
instrumentation and recovery-off flags remain enabled. Both flags are forbidden
in production roles. The driver, link profile and independent literal
vectors/oracles live in `tests/native-input/`; only the binding and native-only
build gates are outside tests. No production role may select either flag, the
compatible or the driver source.
The existing DT event words, bindings, five snapshots, filters, C assertions,
checked initialization and feature/role gates are retained. Two cases exercise
input delivery: `test` uses the actual core physical-layout/event/keymap/HID
Press A/release path; `split_peripheral` retains callbacks and its existing relay
assertions, with no host HID claim. The other three cases retain their original
pre-main terminal-freeze contract. This is an owner input-boundary fixture,
not upstream mock, physical GPIO or radio fidelity evidence.

`native_input.py` requires each callback's literal word, row, column, state,
delay and order and exactly one completion after all callbacks in the two
input-delivery cases, plus the independent Press A/release oracle in `test`.
It also verifies all five compiled
fixture source, effective config/DTS and strict ASan/UBSan application flags.
Actual NSI link options and both ELF runtime-library dependencies are checked;
successful process runs and the precise original-core fault confirm execution
beyond final linkage. Selected input/profile sources are bound into diagnostic
identity, with a fresh configuration nonce and post-link full-ELF hash. Zephyr
owns keymap/DTS/conf reconfiguration dependencies; adding normalized duplicates
of its quoted-include aliases can create an invalid Ninja graph. Additional
selected driver/header/profile dependencies remain owner-managed and tested.
The verifier still requires every original assertion/checked result and the
unchanged snapshots in three independent-flash runs per identical binary.
In `watchdog`, `studio` and `studio_unsecured`, core APPLICATION90 initializes
and enables Kscan, but the original APPLICATION99 test queues a permanent
sysworkq stall at time zero, before the first 10 ms callback. After proving the
freeze, those tests intentionally exit before main. Require the final freeze
PASS before exactly one successful original checked result and exit 0, zero
input/completion/HID records and no main Welcome. Only the newly-added
asynchronous input-completion probe is NOT APPLICABLE there; every original or
unknown checked hook remains mandatory. Do not reorder initialization or the
freeze scenario to make input execute. The literal configured DT word vectors
remain checked even for these three unreachable-delivery cases.
Decoder boundary cases and callback/order/completion/HID rejection controls run
in `test_native_input.py`; a missing or zero-event producer cannot pass merely
because an RPC/peripheral snapshot filters out key logs.

`tests/native-input/run_controls.py` separately builds all five scenarios using
a public DT overlay selecting the original unchanged core mock and a config
fragment disabling only the owner input backend. Original Watchdog role/test
gates remain. Each compiled ELF must exit nonzero on the precise
`kscan_mock.c:103` signed-shift diagnostic in three independent-flash runs.
The runner verifies both core source files against the selected e5 Git objects
before and after; no source rewriting or dependency patch is involved.
Build/setup failures or different diagnostics do not qualify, and expected
control rejections are reported separately from product results.

All five native configurations enable ASan plus UBSan and
`CONFIG_ASAN_RECOVER=n`. The selected Zephyr POSIX backend otherwise appends
`-fsanitize-recover=all` after `EXTRA_CFLAGS`. Verify the final arguments and raw
output; historical recovering exit-0/snapshot matches remain rejected. ASan-only,
optimized, ARM, browser and physical results have separate scopes. No core mock
repair or physical acceptance is claimed.

`bash tests/run-web.sh` is the canonical schema-generation/Web test entry.
It runs all configured Jest scenarios in one worker process. The pre-commit
Jest hook dispatches this same owner entry so nested workers do not multiply
the global container budget. Build and lint retain their separate package entries.

The relay-stack fixture retains all seven relay/three reporter groups and adds
queued-two and running-three requests with exact IDs/status/checkpoint bytes.
A four-slot full queue must return a correlated busy error to the fifth request,
drain the accepted four in order and accept a later retry. The overflow producer
also runs on a 2 KiB Cortex-M stack with the same 344-byte downstream reservation
and 128-byte sampled exception headroom. All cases repeat the identical ELF three
times. Web deletion regressions retain success/explicit backend-error controls,
require feedback for zero/wrong-kind/count/empty results, preserve the retained
row and perform a successful retry. A malformed-head request followed by a
page and status must still run both valid tail entries in order. Test completion
uses a real queue drain: a single `k_work_flush` observes an invocation and
cannot stand in for the full chain of future self-submissions.

The canonical owner also dispatches the separate fairness variant:

```bash
python3 tests/relay-stack/run.py --fairness --zephyr RESOLVED_ZEPHYR \
  --cmsis RESOLVED_CMSIS --nanopb RESOLVED_NANOPB --zmk RESOLVED_DYA \
  --artifacts FRESH_OUTPUT
```

It reuses the same production relay/executor/event boundary and adds the complete
production freeze monitor, trace/capture units and real Zephyr `task_wdt`. A
higher-priority consumer replenishes the four-slot FIFO before each bounded
one-millisecond transport return for four 100ms watchdog periods. IDs remain
ordered, responses continue, the monitored queue itself feeds at least four times
and no freeze is recorded. A real queue blocker then must trigger the timer ISR's
minimal/full freeze publication for that low-priority channel; release must
restore feeding. Kernel watchdog add/feed wrappers observe the real calls and
assert their queue context; they do not replace timer/feed behavior. Pending
publication/reboot and store/transport remain controlled observers. The same
fairness ELF runs three times and retains its identities/logs separately from
the unchanged original stack/reporter variant. Neither establishes actual radio,
Flash, reboot or worst-case stack acceptance. The single-request core transport
wait described in the design is still a separate unresolved boundary.

`tests/web/authorizationLifetime.spec.tsx` keeps the pinned SDK implementation
real above a controlled `call_rpc`: late page after lock, batched lock/unlock
during deletion, old relay answer versus a fresh request, StrictMode setup,
six-second success, actual fifteen-second timeout, ignored late result and retry.
The complete existing Web/codec/privacy/deletion scenarios remain required.

The Web lifetime exposes frozen methods over private authorization/controller
state, so effects and notification callbacks do not mutate memoized values.
StrictMode setup replay and synchronous lock retirement retain the same owner
regressions and the real pinned-SDK fifteen-second deadline checks.
The connected session starts in its loading state before the deferred initial
RPC begins, including after unlock; switching source also marks the scheduled
collection as loading. The existing loading/empty-report test keeps its
immediate loading assertion and its completed empty-state assertion.

Run each reviewed negative control only in a fresh isolated owner source copy:

```bash
python3 tests/review_negative_control.py ISOLATED_OWNER FAULT
```

`relay-drain` restores unbounded FIFO draining; the full fairness variant must
fail its feed/no-freeze assertions in all three runs while still completing
responses and detecting the genuine-stall control. `fifo-overwrite` discards
admitted bytes; the original relay-stack variant must reject its queued IDs and
full-FIFO assertions in all three runs. `web-lock` disables only core lock
handling and must fail the authorization lifetime cases. `sdk-timeout` restores
the pinned SDK's five-second budget and must fail the six-second success and
fifteen-second boundary assertions. `delete-zero` restores silent zero-count
deletion and must fail the retained-row feedback case. Run the Web faults through
`bash tests/run-web.sh`, with the existing locked dependencies. Source selectors
must match exactly once. Compile/setup failures, collection errors, timeouts and
unclassified failures are not accepted as killed controls; preserve ordered raw
output and actual assertion failures. Existing arithmetic/store/context/info-log
and native admission controls remain required. These remain controlled checks,
with no new physical acceptance claim.

## Task WDT default ownership

`python3 tests/task_wdt_defaults.py --workspace RESOLVED_WORKSPACE --output FRESH_OUTPUT`
configures the unchanged real DYA/Zephyr XIAO graph. Nine cases check the
10000-ms monitor default, explicit fallback y/n, external/disabled-owner and
freeze-disabled defaults, explicit minimum override, the 10-ms boundary,
9999 ms and the 600000-ms deadline with a 10000-ms cap. A test-only earlier
100-ms Kconfig default must configure and fail the named minimum assertion;
configure/compiler errors do not qualify. These checks establish Kconfig,
including the preserved explicit n value, without claiming that n is runtime-safe.

`python3 tests/task-wdt-idle/run.py --workspace RESOLVED_WORKSPACE --output FRESH_OUTPUT`
compiles the actual owner freeze initializer/feed work, unchanged ZMK workqueue
and actual Zephyr Task WDT/kernel under strict ASan/UBSan and 64-bit timeouts.
One- and two-queue binaries each run three times. Link wrappers only observe
real timer starts during actual initialization/feeds; all calls reach Zephyr.
The 1.2 s idle window admits no 100-ms background expiries, then the real 5 s
feeds must restart the 10 s deadline. The fixture's explicit selected values
are separate from the real full-board Kconfig default proof above.
The old-100ms control must fail the named idle assertion. The original
fallback-off choice must exit on the specified `task_wdt.c:79` signed-overflow
diagnostic. Both controls build and run three times; build failures, other
assertions, missing completion and unrelated diagnostics do not qualify.

The complete `test_owner.py` dispatches both entries and retains the original
seven board/role artifacts plus explicit minimum, external Task WDT and
freeze-disabled artifacts. Normal monitors require fallback y/minimum10000,
with original trace/diagnostic/resource assertions. The preblocked-sysworkq
arming fixture retains its real ISR/deadline assertions with a finite 100-ms
minimum matching its 100-ms channel; the same ELF runs three times.
No patched core supplies any of these assertions. The original upstream mock
control still encounters the documented kscan shift; owner input-fixture success
does not repair it or establish physical acceptance.

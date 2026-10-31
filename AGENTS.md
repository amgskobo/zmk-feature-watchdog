# Watchdog: agent guide

Own fault/freeze capture, incident storage, reset/arming, split retrieval
and the incident decoder/Web client.
This owner uses DYA's unofficial custom Studio RPC; stock ZMK is NOT APPLICABLE.

## Publication privacy and local execution

This rule applies to information prepared for publication or external sharing.
During an authorized task, agents may inspect and use actual local paths,
configuration, host/terminal/device identities and private test evidence as
needed for implementation, diagnosis, identity matching and validation. Such
local access does not itself authorize collection outside the task or disclosure.

Before preparing a public commit, push, PR, release, documentation/site output
or CI artifact, review the actual publication payload, including source,
comments, AGENTS files, examples, test fixtures, logs, screenshots, generated
files and metadata. Exclude user-related names and preferences, private absolute
paths and local-environment details, workstation/terminal identities, individual
device labels, ports, serials, addresses, pairing/bond identities, credentials,
and user- or device-specific test/measurement histories. Also check generated
and uploaded artifacts; checking source prose alone is insufficient.

Use repository-relative paths, documented variables/placeholders, neutral names
and clearly synthetic fixtures in public material. Declared public project
identifiers, existing attribution, standard API/product/platform names and
reproducible support requirements remain technical project information; do not
present one user's actual environment as a project requirement. In particular,
documenting supported platforms is distinct from identifying a user's device.

Keep exact originals and identifiers in appropriate private/local task records
when required for accurate evidence or device matching. Produce a separate
sanitized publication copy without weakening assertions or inventing results;
do not describe it as the unchanged raw record. If useful evidence cannot be
shared without exposing these details, keep it private and state the public
validation limit. Recheck the final publication snapshot and its artifacts.
Existing authorization requirements for version-control, publication, device
writes, resets and data erasure still apply.

## Before editing

Do not commit or create/update PRs without an explicit user request.
Keep changes local and preserve unrelated work. Staging, pushes, merges, tags,
publication, flashing and settings erasure also need explicit authorization.
Dependency instructions grant none.

Read [README.md](README.md), [tests/README.md](tests/README.md) and
[tests/maintenance.json](tests/maintenance.json) before relevant work.
Resolve the selected scope's actual west imports, local overrides, Kconfig,
backend and dependency revisions. Read applicable dependency instructions and
required documents once per checkout/revision; record READ, NO AGENTS or
INACCESSIBLE. A sibling checkout does not identify a container dependency.
Block only dependent checks; do not update pins or caches implicitly.

## Module contract

- Preserve store chronology, replacement/drop accounting, failed-write recovery
  and bounded relay stack/response storage. Verify actual ordered pages.
- Report only implemented observations and explicit measurement gaps. Retain
  source/boot/build/record identities, raw times, age and retrieval errors.
  Successful retrieval or no error logs alone is not a stability verdict.
- For wire features, update schema, firmware and Web client together, in that
  order. Set nanopb submessage `has_*` flags. The Studio 32-bit nanopb mode
  does not support 64-bit proto fields; use `uint32`/`int32`.
- Keep setup instructions current. Use Custom Settings for module parameters
  where supported; do not add a second persistence owner.

## Checks

- Documentation only: `git diff --check` and changed links/command references.
- Firmware: `python3 -m unittest discover -v -s tests -p test_owner.py`
  in the resolved DYA workspace; retain all native/board/role variants and
  verify actual exits, registered test completion and compiled feature gates.
- Web: `bash tests/run-web.sh`; use the locked dependencies and documented
  lint/build entries. Keep Web assertions in `tests/`, including package dispatch.
- Follow `tests/README.md` for focused protocol/kernel/negative controls.
  Do not substitute raw west build success for the owner's result validator.

## Test and evidence rules

Keep executable assertions, fixtures, models, mutations and validators in
this owner's `tests/`; CI/package scripts dispatch them. Guarded production
probes expose state, not scenarios. Cover normal/boundary/invalid behavior,
independent state, reset/cancel, recovery and role/protocol gates; map families
to named fixtures and gaps. Regressions need a failing original/control.
Preserve unique assertions and external retirement maps before removing tests.

Use fresh outputs; an existing directory stops the run. Record commands,
source/dependency/feature/backend identities, actual exits and raw ordered times.
Use exact fixed-input traces or specified timing bounds; repeat identical
binary/input/seed/PHY/initial-flash runs at least three times before claiming
nondeterminism. Classify snapshots before updates; never accept just to pass.
Report PASS, FAIL, BLOCKED, NOT RUN and unsupported NOT APPLICABLE separately.
Host/native/build checks do not establish physical HID/transport/driver/timing/
power/reset acceptance. Do not patch dependencies, disable required features or
weaken assertions to pass. Keep necessary diffs/failures, not rolling backups.

## Container test execution

Run automated tests, builds and dependency setup inside Docker. Host entry points
may launch containers and export results; do not run test runtimes or install test
packages/toolchains on the host. Owner source may be mounted read-only. Keep
test-only ZMK/Zephyr/tool checkouts, runtime environments, temporary files, build
outputs and caches in container images or named volumes. Tests must not require
host sibling dependency checkouts or private absolute paths.

Resolve and verify the selected revisions inside the container. Prepare missing
dependencies explicitly in an isolated container workspace; do not silently update
shared caches or pins. Preserve all owner scenarios, assertions and required
matrices. Hardware acceptance needs documented device access; report unavailable
container transports as BLOCKED rather than substituting host execution or mocked
acceptance. Export only reviewed evidence and retain the publication privacy rule.

## Fast container test execution

Use prepared container environments, persistent caches and bounded parallelism
to reduce execution time. Preserve every required scenario, assertion, negative
control, sanitizer, supported-core variant and repeat.

- Separate environment preparation from test execution. Pin container images by
  digest and resolve dependency revisions explicitly. Reuse prepared environments
  only when their image, toolchain, dependency revisions and lockfiles match.
  Do not fetch or update dependency inputs during an ordinary test run. Missing
  inputs block only dependent checks.

- Keep dependency checkouts, installed test environments, temporary files, build
  directories and caches in container images or named volumes. Prefer Linux
  filesystem storage for intensive file access. Any staged owner sources must
  preserve all required test/compiler inputs, including relevant untracked files,
  with source identity and pre/post-run drift verification.

- Reuse explicitly selected compiler and package caches with their supported
  identity and concurrency checks. Keep incompatible environments separate.
  Never share writable build directories or mutable installed dependency trees
  between independent jobs. Preserve required clean-cache checks; use isolated
  fresh caches for those checks.

- Dispatch each owner's canonical test entries from inside the container.
  Common orchestration may select, schedule and collect those entries; assertions
  and fixtures remain in the owning repository. Avoid nested Docker launches and
  repeated environment preparation for compatible jobs. Isolate each job's
  environment, working files and outputs.

- Enforce one configurable CPU and memory budget across concurrent suites,
  targets, compiler jobs and test workers. Account for nested parallelism.
  Parallelize only independent jobs; preserve required execution and output order.

- Within a run, build each required binary once per distinct input/configuration
  variant and reuse that exact binary for required identical-condition repeats,
  with each repeat's required initial state. Execute every required test; cached
  artifacts or previous PASS records do not establish a new test execution.

- Keep fresh build and evidence directories. Record actual commands, exits,
  completion markers, ordered output, source/dependency/image identities and
  cache configuration. Measure preparation, staging, build, execution and
  validation separately.

Quick and full retain their existing contracts. Interface/infrastructure changes
still require the complete owner matrix. Evaluate speed changes using equivalent
inputs and checks, report cold and warm runs separately, and retain failures.

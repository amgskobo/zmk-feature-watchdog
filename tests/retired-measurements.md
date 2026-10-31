# Retired Watchdog measurements

The retired interface covers scheduler/queue/key/input/BLE performance
collectors, health snapshot storage/readback, their enable option, health/latency
UI/polling/graphs, standalone input capture and offline health report tooling.
Incident/freeze/fatal/reset/trace/operation-checkpoint/runtime-detector status
contracts remain. USB telemetry is a separate owner.

The exact pre-removal source and unique assertions are hash-verified in
`$WATCHDOG_RETIREMENT_ARCHIVE/sources-and-tests.zip`;
the adjacent manifest lists every archived hash. Prior native/Web/physical
results and unresolved gaps retain their original identities and are not
reclassified as passing current measurement checks.
`WATCHDOG_RETIREMENT_ARCHIVE` denotes the privately retained retirement archive.
Its actual location and original identity manifest stay in local review records.

| Retired assertions | Current disposition |
| --- | --- |
| `tests/performance/`: seven real-kernel role/order/system/BLE ABI variants, timing/counter/wire/history/admission/storage bounds | RETIRED, complete source archived; collectors removed |
| Health decoder/report tests and partial/stale/identity/window comparisons | RETIRED; offline health tool removed |
| Web health/polling/trend/latency/capture suites and browser preview | RETIRED; components/tools and their UI entry removed |
| Relay maximum health page/padding payload | RETIRED; replaced only at the transport boundary by explicit legacy-health rejection, without sampling/storage |

Retired protobuf field numbers and names are reserved, and health message types
are removed. An old GetHealth request decodes as no supported request and gets
`Unsupported or missing watchdog request` locally, before split dispatch. No
sampling or storage is enabled. Incident/status field numbers and stored record
formats are unchanged.

`test_retirement.py` rejects the archived collector/configuration/UI source as a
negative control. The existing Cortex-M relay fixture retains seven incident
groups, three boot-report groups and its real DYA event/nanopb/2KiB stack bound;
it additionally checks ordinary rejection of the legacy request. Retained Web tests cover no
measurement UI/background requests as well as existing fault/status contracts.
These guards do not replace the retired feature's algorithm assertions.
Actual board HID, BLE delivery, retention, power and reset gates remain open.

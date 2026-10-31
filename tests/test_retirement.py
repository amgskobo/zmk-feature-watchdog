"""Selection guards for removal of Watchdog performance/health instrumentation."""

from pathlib import Path
import unittest


def retirement_errors(root):
    errors = []
    for name in (
        "watchdog_performance.c",
        "watchdog_ble_performance.c",
        "watchdog_health.c",
    ):
        if (root / "src" / name).exists():
            errors.append(name)
    for name in ("Kconfig", "CMakeLists.txt", "src/watchdog_freeze.c"):
        if "ZMK_WATCHDOG_PERFORMANCE" in (root / name).read_text():
            errors.append(name)
    if (root / "web/src/HealthSection.tsx").exists():
        errors.append("HealthSection.tsx")
    return errors


class RetirementTests(unittest.TestCase):
    def test_no_measurement_sources_or_enable_switch(self):
        self.assertEqual(retirement_errors(Path(__file__).resolve().parents[1]), [])

    def test_legacy_requests_have_no_sampler_or_relay_admission(self):
        root = Path(__file__).resolve().parents[1]
        for name in (
            "src/studio/watchdog_handler.c",
            "src/studio/watchdog_request_exec.c",
            "src/split/watchdog_relay_central.c",
            "src/split/watchdog_relay_peripheral.c",
        ):
            source = (root / name).read_text(encoding="utf-8")
            self.assertNotIn("get_health", source)
            self.assertNotIn("_health_tag", source)
            self.assertNotIn("zmk_watchdog_health_", source)

    def test_legacy_wire_numbers_stay_reserved(self):
        proto = (
            Path(__file__).resolve().parents[1]
            / "proto/cormoran/watchdog/watchdog.proto"
        ).read_text(encoding="utf-8")
        self.assertNotIn("GetHealthRequest", proto)
        self.assertNotIn("HealthResponse health", proto)
        self.assertIn('reserved 5;\n    reserved "get_health";', proto)
        self.assertEqual(proto.count('reserved 7;\n    reserved "health";'), 2)


if __name__ == "__main__":
    unittest.main()

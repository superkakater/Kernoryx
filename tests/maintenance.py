#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "maintenance", Path(__file__).resolve().parents[1] / "scripts/maintenance.py"
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class MaintenanceTests(unittest.TestCase):
    def test_memory_pressure_not_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "pressure").mkdir()
            (p / "meminfo").write_text("MemTotal: 1000 kB\nMemAvailable: 800 kB\nCached: 700 kB\n")
            (p / "pressure/memory").write_text(
                "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
            )
            self.assertIn("not observed", m.memory_status(p))
            (p / "pressure/memory").write_text(
                "some avg10=20.00 avg60=0.00 avg300=0.00 total=1\nfull avg10=2.00 avg60=0.00 avg300=0.00 total=1\n"
            )
            self.assertIn("stalls: high", m.memory_status(p))
            (p / "pressure/memory").unlink()
            self.assertIn("PSI unavailable", m.memory_status(p))

    def test_reclaim_scope_and_dry_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "cgroup.controllers").write_text("memory")
            (p / "work").mkdir()
            target = p / "work/memory.reclaim"
            target.write_text("")
            self.assertIn("DRY RUN", m.memory_reclaim("work", "1M", True, p))
            self.assertEqual(target.read_text(), "")
            m.memory_reclaim("work", "1M", False, p)
            self.assertEqual(target.read_text(), "1M")
            for group in ["/", "../work", ".", "work/../work"]:
                with self.assertRaises(ValueError):
                    m.memory_reclaim(group, "1M", False, p)
            for amount in ["0", "-1", "1T", "9G", "2M\n1G"]:
                with self.assertRaises(ValueError):
                    m.memory_reclaim("work", amount, False, p)
            target.unlink()
            target.symlink_to(p / "cgroup.controllers")
            with self.assertRaises(ValueError):
                m.memory_reclaim("work", "1M", False, p)

    def test_cache_age_and_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(m, "user_required"):
            p = Path(tmp)
            cache = p / ".cache/app"
            cache.mkdir(parents=True)
            old = cache / "old"
            new = cache / "new"
            outside = p / "keep"
            old.write_text("old")
            new.write_text("new")
            outside.write_text("safe")
            past = time.time() - 86400 * 90
            os.utime(old, (past, past))
            (cache / "link").symlink_to(outside)
            (cache / "dirlink").symlink_to(p, target_is_directory=True)
            with patch.object(m, "HOME", p):
                m.cache_clean(cache, 30, False)
                self.assertTrue(old.exists())
                m.cache_clean(cache, 30, True)
                self.assertFalse(old.exists())
                self.assertTrue(new.exists())
                self.assertEqual(outside.read_text(), "safe")
                for bad in [p, p / ".cache", p / ".cache/app/../../", cache / "dirlink"]:
                    with self.assertRaises((ValueError, OSError)):
                        m.cache_clean(bad, 30, True)

    def test_root_user_cleanup_rejected(self):
        with patch.object(m.os, "geteuid", return_value=0):
            with self.assertRaises(ValueError):
                m.cache_clean("/root/.cache/test", 30, True)
            with self.assertRaises(ValueError):
                m.trash_clean(30, False)

    def test_schema_before_side_effects(self):
        valid = {
            "version": 1,
            "actions": [
                {"type": "cleanup", "provider": "journal", "days": 30},
                {"type": "mode", "mode": "beast", "for": "30m"},
            ],
        }
        m.validate_workflow(valid)
        for invalid in [
            {"version": 1, "actions": [{"type": "shell", "command": "touch /bad"}]},
            {"version": 1, "actions": [{"type": "mode", "mode": "beast", "for": "3h"}]},
            {"version": 1, "actions": [{"type": "cleanup", "provider": "journal", "path": "/etc"}]},
            {"version": 1, "actions": list(reversed(valid["actions"]))},
            {"version": 1, "actions": [{"type": "mode", "mode": "eco", "for": "2m", "replace": "yes"}]},
        ]:
            with patch.object(m, "action_run") as action:
                with self.assertRaises(ValueError):
                    m.workflow_run(invalid, False)
                action.assert_not_called()

    def test_experimental_fan_workflow(self):
        action = {"type": "fan", "for": "30s", "experimental": True}
        m.validate_action(action)
        with patch.object(m, "daemon", return_value="preview") as daemon:
            m.action_run(action, True)
            daemon.assert_called_once_with(["fan", "boost", "--for", "30s", "--experimental", "--dry-run"])
        action["experimental"] = "yes"
        with self.assertRaises(ValueError):
            m.validate_action(action)

    def test_workflow_failure_stops(self):
        data = {
            "version": 1,
            "actions": [
                {"type": "cleanup", "provider": "journal"},
                {"type": "mode", "mode": "beast", "for": "30m"},
            ],
        }
        with patch.object(m, "action_run", side_effect=ValueError("failed")) as action:
            with self.assertRaises(ValueError):
                m.workflow_run(data, False)
            self.assertEqual(action.call_count, 1)

    def test_native_preview_precedes_mutation(self):
        calls = []
        with patch.object(m, "native", return_value="/usr/bin/paccache"), patch.object(
            m, "root_required"
        ), patch.object(m, "run", side_effect=lambda a: calls.append(a) or ""):
            m.cleanup("packages", days=30, keep=3, apply=False)
            self.assertEqual(len(calls), 1)
            self.assertIn("-d", calls[0])
            m.cleanup("packages", days=30, keep=3, apply=True)
            self.assertIn("-d", calls[1])
            self.assertIn("-r", calls[2])
        with patch.object(m, "native", return_value="/usr/bin/systemd-tmpfiles"), patch.object(
            m, "run", return_value="old version"
        ):
            with self.assertRaises(ValueError):
                m.cleanup("temp", apply=True)

    def test_schedule_injection_and_catchup(self):
        svc, timer = m.timer_text("weekly", "weekly", True, "/usr/local/bin/kernoryx")
        self.assertIn("Persistent=true", timer)
        self.assertIn("Type=oneshot", svc)
        for name in ["../x", "a\nExecStart=bad", "a.service", "UPPER"]:
            with self.assertRaises(ValueError):
                m.timer_text(name, "weekly", False, "/usr/bin/kernoryx")
        for calendar in ["weekly\nUnit=bad", "%u", "", "weekly\\"]:
            with self.assertRaises(ValueError):
                m.timer_text("valid", calendar, False, "/usr/bin/kernoryx")
        from argparse import Namespace

        data = {"version": 1, "actions": [{"type": "mode", "mode": "beast", "for": "1m"}]}
        with patch.object(m, "trusted_json", return_value=data):
            with self.assertRaises(ValueError):
                m.schedule_install(Namespace(file="x", persistent=True))

    def test_config_permissions(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "test.json"
            p.write_text('{"version":1,"actions":[]}')
            self.assertEqual(m.trusted_json(p)["version"], 1)
            p.chmod(0o666)
            with self.assertRaises(ValueError):
                m.trusted_json(p)
            p.chmod(0o600)
            link = Path(tmp) / "link"
            link.symlink_to(p)
            with self.assertRaises(OSError):
                m.trusted_json(link)


if __name__ == "__main__":
    unittest.main()

import tempfile
import unittest
from pathlib import Path

from collect_aot_profile import parse_workload, workload_command


class CollectAotProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.config = self.root / "config"
        self.config.mkdir()
        (self.config / "settings.json").write_text("{}", encoding="utf-8")
        self.template = [
            "old.exe", "--title-plugin", "old.dll", "--config", str(self.config),
            "--load-state", "field.oot3dsav", "--throughput-benchmark",
            "--input-timeline", "neutral.json", "--frames", "1800",
            "--max-seconds", "90", "--output", "old.json",
            "--renderer-cache-directory", "old-cache",
        ]

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def command(self, timeline: Path | None) -> list[str]:
        run = self.root / "run"
        run.mkdir()
        return workload_command(self.template, Path("new.exe"), Path("pgo.dll"),
                                Path("mill.oot3dsav"), timeline, 900, 270, run,
                                self.root / "cache")

    def test_replaces_state_module_and_run_limits(self) -> None:
        command = self.command(None)
        self.assertEqual(command[0], "new.exe")
        for key, value in (("--title-plugin", "pgo.dll"), ("--load-state", "mill.oot3dsav"),
                           ("--input-timeline", "neutral.json"), ("--frames", "900"),
                           ("--max-seconds", "270")):
            self.assertEqual(command.count(key), 1)
            self.assertEqual(command[command.index(key) + 1], value)
        self.assertIn("--throughput-benchmark", command)
        self.assertNotIn("field.oot3dsav", command)

    def test_uses_private_configuration_copy_and_explicit_timeline(self) -> None:
        command = self.command(Path("dialogue.json"))
        copied = Path(command[command.index("--config") + 1])
        self.assertNotEqual(copied, self.config)
        self.assertTrue((copied / "settings.json").is_file())
        self.assertEqual(command[command.index("--input-timeline") + 1], "dialogue.json")

    def test_parses_optional_timeline(self) -> None:
        self.assertEqual(parse_workload("a.oot3dsav"), (Path("a.oot3dsav"), None))
        self.assertEqual(parse_workload("C:/a.oot3dsav::C:/t.json"),
                         (Path("C:/a.oot3dsav"), Path("C:/t.json")))


if __name__ == "__main__":
    unittest.main()

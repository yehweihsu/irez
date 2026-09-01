import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from diff_test import normalize  # noqa: E402


class NormalizePathAliasesTest(unittest.TestCase):
    def test_windows_short_state_path_matches_resolved_long_path(self):
        short = Path(r"C:\Users\RUNNER~1\AppData\Local\Temp\tmp123\cpp-state")
        long = Path(r"C:\Users\runneradmin\AppData\Local\Temp\tmp123\cpp-state")
        long_forward = str(long).replace("\\", "/")
        payload = {
            "state_dir": long_forward,
            "error": f"tool: {long_forward}/artifacts/input.ll",
        }

        with patch.object(Path, "resolve", return_value=long):
            self.assertEqual(
                normalize(payload, short),
                {
                    "error": "tool: <STATE>/artifacts/input.ll",
                    "state_dir": "<STATE>",
                },
            )


if __name__ == "__main__":
    unittest.main()

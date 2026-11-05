"""Portable contract checks for the legacy Windows config migration batch file."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
MIGRATION_SCRIPT = ROOT / "src_assets/windows/misc/migration/migrate-config.bat"


class MigrationScriptTests(unittest.TestCase):
    def test_migration_deletes_only_the_named_legacy_log(self):
        lines = MIGRATION_SCRIPT.read_text(encoding="utf-8").splitlines()
        delete_commands = [
            line.strip()
            for line in lines
            if re.match(r"\s*(?:if\s+exist\s+\S+\s+)?del\b", line, re.IGNORECASE)
        ]
        self.assertEqual(
            delete_commands,
            ['if exist "%OLD_DIR%\\sunshine.log" del "%OLD_DIR%\\sunshine.log"'],
        )
        self.assertNotRegex(
            "\n".join(delete_commands),
            r"(?i)\.(?:txt|log)\b.*\*|\*.*\.(?:txt|log)\b",
        )

    def test_apps_path_is_built_from_the_inherited_environment_value(self):
        script = MIGRATION_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("Join-Path $env:NEW_DIR 'apps.json'", script)
        self.assertNotIn("'%NEW_DIR%\\apps.json'", script)


if __name__ == "__main__":
    unittest.main()

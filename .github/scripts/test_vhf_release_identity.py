import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("identity", Path(__file__).with_name("vhf_release_identity.py"))
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class ProducerIdentityTests(unittest.TestCase):
    def contract(self, source):
        values = (*identity.RELEASES[source], source)
        return "\n".join(f'set(SUNSHINE_VHF_GAMEPAD_{field}\n "{value}" CACHE STRING "pin")'
                         for field, value in zip(identity.FIELDS, values))

    def test_audited_sources_keep_their_own_release(self):
        for source, release in identity.RELEASES.items():
            with self.subTest(source=source):
                selected = identity.select_identity(source, self.contract(source))
                self.assertEqual(selected["VHF_SOURCE_REVISION"], source)
                self.assertEqual(selected["VHF_TAG"], release[1])
                self.assertEqual(selected["VHF_ARCHIVE_SHA256"], release[2])

    def test_every_mismatched_pin_fails_closed(self):
        for source in identity.RELEASES:
            contract = self.contract(source)
            for value in (*identity.RELEASES[source], source):
                with self.subTest(source=source, value=value), self.assertRaises(ValueError):
                    identity.select_identity(source, contract.replace(f'"{value}"', '"different"'))

    def test_missing_duplicate_and_interpolated_pins_fail_closed(self):
        source = next(iter(identity.RELEASES))
        contract = self.contract(source)
        field = "SUNSHINE_VHF_GAMEPAD_REPOSITORY"
        for modified in (contract.replace(field, "UNRELATED"), contract + "\n" + contract,
                         contract.replace('"Nonary/libvirtualgamepad"', '"${REPOSITORY}"')):
            with self.subTest(modified=modified), self.assertRaises(ValueError):
                identity.select_identity(source, modified)

    def test_unknown_source_is_never_inferred_from_metadata(self):
        with self.assertRaises(ValueError):
            identity.select_identity("f" * 40, self.contract(next(iter(identity.RELEASES))))


if __name__ == "__main__":
    unittest.main()

"""Future-kernel preparation policy; no real containers, keys or deployments."""
import base64
import importlib.util
import io
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT / 'packaging/linux/bazzite/drivers'


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, KIT / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load('update_builder', 'build-drivers.py')
signer = load('update_signer', 'sign-modules.py')
IMAGE = 'ghcr.io/ublue-os/bazzite-nvidia-open@sha256:' + 'a' * 64
FUTURE = '7.2.12-ogc5.1.fc44.x86_64'


class UpdatePolicy(unittest.TestCase):
    def test_moving_tags_wrong_variants_and_nonregistry_inputs_rejected(self):
        for image in ('ghcr.io/ublue-os/bazzite-nvidia-open:stable',
                      IMAGE.replace('nvidia-open', 'deck'), IMAGE + ';id',
                      'localhost/test@sha256:' + 'a' * 64):
            with self.subTest(image=image), self.assertRaises(RuntimeError):
                builder.validate_image(image)
        self.assertEqual(builder.validate_image(IMAGE), IMAGE)

    def test_future_kernel_allowed_but_traversal_and_unsupported_kernels_rejected(self):
        self.assertEqual(builder.validate_kernel(FUTURE), FUTURE)
        for kernel in ('../../tmp', '6.15.9-test.x86_64', FUTURE + '/bad', '7.2.12-test.aarch64'):
            with self.subTest(kernel=kernel), self.assertRaises(RuntimeError):
                builder.validate_kernel(kernel)

    def test_probe_uses_pinned_image_offline_without_host_mounts(self):
        metadata = {'kernel': FUTURE, 'dependency': 'kernel-core = 7.2.12-ogc5.1.fc44'}
        with patch.object(builder, 'run') as pull, \
             patch.object(builder.subprocess, 'check_output', return_value=json.dumps(metadata)) as probe:
            self.assertEqual(builder.probe_image(IMAGE), metadata)
            pull.assert_called_once_with('podman', 'pull', IMAGE)
            command = probe.call_args.args[0]
            self.assertIn('--network=none', command)
            self.assertIn('--cap-drop=ALL', command)
            self.assertNotIn('--volume', command)
            self.assertIn(IMAGE, command)

    def test_missing_headers_or_compiler_stop_preparation(self):
        with patch.object(builder, 'run'), \
             patch.object(builder.subprocess, 'check_output', side_effect=subprocess.CalledProcessError(1, 'probe')):
            with self.assertRaises(subprocess.CalledProcessError):
                builder.probe_image(IMAGE)

    def test_development_package_or_injected_dependency_rejected(self):
        for dependency in ('kernel-devel = 7.2.12', 'kernel-core = 7.2.12\nRequires: bad'):
            with patch.object(builder, 'run'), \
                 patch.object(builder.subprocess, 'check_output', return_value=json.dumps({'kernel': FUTURE, 'dependency': dependency})):
                with self.assertRaises(RuntimeError):
                    builder.probe_image(IMAGE)

    def test_staging_command_replaces_driver_and_retains_signature_policy(self):
        command = shlex.split(builder.update_command(IMAGE, Path('/home/Jay/build output/driver.rpm')))
        self.assertIn('--install=/home/Jay/build output/driver.rpm', command)
        self.assertIn('--uninstall=vibepollo-kmod', command)
        self.assertEqual(command[-1], 'ostree-image-signed:docker://' + IMAGE)
        self.assertNotIn('--reboot', command)

    def test_future_rpm_depmod_and_dependency_match_target_not_running_kernel(self):
        spec = builder.make_spec(FUTURE, [], 'kernel-core = 7.2.12-ogc5.1.fc44', '3.bazzite.future')
        self.assertIn('Release: 3.bazzite.future', spec)
        self.assertIn('/usr/sbin/depmod -a ' + FUTURE, spec)
        self.assertNotIn(builder.KERNEL, spec)

    def test_update_does_not_generate_a_new_unenrolled_signing_key(self):
        request = {'kernel': FUTURE, 'modules': {name: base64.b64encode(b'\x7fELFtest').decode() for name in signer.NAMES},
                   'require_existing_key': True}
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(signer.os, 'geteuid', return_value=0), \
             patch.object(signer, 'KEYDIR', Path(temporary)), \
             patch.object(Path, 'resolve', return_value=Path('/trusted/sign-file')), \
             patch.object(signer, 'trusted'), \
             patch.object(signer, 'run') as execute, \
             patch.object(sys, 'stdin', SimpleNamespace(buffer=io.BytesIO(json.dumps(request).encode()))):
            with self.assertRaisesRegex(RuntimeError, 'existing enrolled signing key'):
                signer.main()
            execute.assert_not_called()


if __name__ == '__main__':
    unittest.main()

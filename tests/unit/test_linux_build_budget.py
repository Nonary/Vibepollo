import importlib.util
from pathlib import Path
import unittest
import tempfile

spec = importlib.util.spec_from_file_location('budget', Path(__file__).parents[2] / 'scripts' / 'linux-build-budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


class BudgetTests(unittest.TestCase):
    def test_standard_public_runner(self):
        self.assertEqual(budget.worker_count(4, 16 * 1024**3), 4)

    def test_private_runner(self):
        self.assertEqual(budget.worker_count(2, 8 * 1024**3), 2)

    def test_memory_pressure(self):
        self.assertEqual(budget.worker_count(16, 5 * 1024**3), 1)

    def test_container_quota(self):
        self.assertEqual(budget.worker_count(32, 64 * 1024**3, 2), 2)

    def test_small_runner_keeps_one_worker(self):
        self.assertEqual(budget.worker_count(1, 1024**3), 1)

    def test_detect_v2_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'meminfo').write_text('MemAvailable: 16777216 kB\n')
            (root / 'memory.max').write_text(str(8 * 1024**3))
            (root / 'memory.current').write_text(str(1024**3))
            (root / 'cpu.max').write_text('200000 100000')
            _, available, quota = budget.detect(root / 'meminfo', root, root / 'absent-process', root / 'absent-mounts')
            self.assertEqual(available, 7 * 1024**3)
            self.assertEqual(quota, 2)

    def test_detect_v1_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'meminfo').write_text('MemAvailable: 16777216 kB\n')
            (root / 'memory').mkdir()
            (root / 'cpu').mkdir()
            (root / 'memory/memory.limit_in_bytes').write_text(str(4 * 1024**3))
            (root / 'memory/memory.usage_in_bytes').write_text(str(1024**3))
            (root / 'cpu/cpu.cfs_quota_us').write_text('100000')
            (root / 'cpu/cpu.cfs_period_us').write_text('100000')
            _, available, quota = budget.detect(root / 'meminfo', root, root / 'absent-process', root / 'absent-mounts')
            self.assertEqual(available, 3 * 1024**3)
            self.assertEqual(quota, 1)

    def test_nested_v2_inherits_stricter_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child = root / 'group/child'
            child.mkdir(parents=True)
            (root / 'meminfo').write_text('MemAvailable: 16777216 kB\n')
            (root / 'self-cgroup').write_text('0::/group/child\n')
            (root / 'mountinfo').write_text(f'1 0 0:1 / {root.as_posix()} rw - cgroup2 cgroup rw\n')
            (root / 'cpu.max').write_text('max 100000')
            (child / 'cpu.max').write_text('400000 100000')
            (child.parent / 'cpu.max').write_text('200000 100000')
            (child.parent / 'memory.max').write_text(str(6 * 1024**3))
            (child.parent / 'memory.current').write_text(str(1024**3))
            _, available, quota = budget.detect(root / 'meminfo', root, root / 'self-cgroup', root / 'mountinfo')
            self.assertEqual(available, 5 * 1024**3)
            self.assertEqual(quota, 2)

    def test_namespace_mount_root_and_missing_controller(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child = root / 'child'
            child.mkdir()
            (root / 'meminfo').write_text('MemAvailable: 16777216 kB\n')
            (root / 'self-cgroup').write_text('0::/child\n')
            (root / 'mountinfo').write_text(f'1 0 0:1 /host/container {root.as_posix()} rw - cgroup2 cgroup rw\n')
            (child / 'cpu.max').write_text('200000 100000')
            # A missing usage controller must fail conservatively.
            (child / 'memory.max').write_text(str(6 * 1024**3))
            _, _, quota = budget.detect(root / 'meminfo', root, root / 'self-cgroup', root / 'mountinfo')
            self.assertEqual(quota, 1)

    def test_mountinfo_escaped_space(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mount = root / 'mount space'
            mount.mkdir()
            (root / 'meminfo').write_text('MemAvailable: 16777216 kB\n')
            (root / 'self-cgroup').write_text('0::/\n')
            escaped = mount.as_posix().replace(' ', '\\040')
            (root / 'mountinfo').write_text(f'1 0 0:1 / {escaped} rw - cgroup2 cgroup rw\n')
            (mount / 'cpu.max').write_text('200000 100000')
            _, _, quota = budget.detect(root / 'meminfo', root / 'unused', root / 'self-cgroup', root / 'mountinfo')
            self.assertEqual(quota, 2)


if __name__ == '__main__':
    unittest.main()

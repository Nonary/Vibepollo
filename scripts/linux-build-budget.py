#!/usr/bin/env python3
"""Bound Linux build workers by the resources actually available to this job."""
import os
import sys
import re
from pathlib import Path


def worker_count(cpus, available_bytes, quota_cpus=None):
    # Reserve 2 GiB for the OS, linker and packaging. Each compile/LTO worker
    # gets 2 GiB; one worker remains possible on smaller runners.
    memory_workers = max(1, (available_bytes - 2 * 1024**3) // (2 * 1024**3))
    return max(1, min(cpus, quota_cpus or cpus, memory_workers))


def controller_paths(root, process_path, mountinfo_path):
    paths = [(root, 'v2'), (root / 'memory', 'memory'), (root / 'cpu', 'cpu')]
    if not Path(process_path).exists() or not Path(mountinfo_path).exists():
        return paths
    groups = []
    for line in Path(process_path).read_text().splitlines():
        _, controllers, location = line.split(':', 2)
        groups.append((set(controllers.split(',')), Path(location)))
    for line in Path(mountinfo_path).read_text().splitlines():
        before, _, after = line.partition(' - ')
        fields, filesystem = before.split(), after.split()
        if len(fields) < 5 or len(filesystem) < 3 or filesystem[0] not in {'cgroup', 'cgroup2'}:
            continue
        decode = lambda value: re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), value)
        mount_root, mount_point = Path(decode(fields[3])), Path(decode(fields[4]))
        controllers = set(filesystem[2].split(','))
        for group_controllers, location in groups:
            kinds = ['v2'] if filesystem[0] == 'cgroup2' and group_controllers == {''} else sorted(group_controllers & controllers & {'cpu', 'memory'})
            if not kinds:
                continue
            try:
                relative = location.relative_to(mount_root)
            except ValueError:
                # In a cgroup namespace /proc/self/cgroup is relative to its
                # namespace root even when mountinfo exposes a host root.
                relative = location.relative_to('/')
            current = mount_point / relative
            if '..' in relative.parts:
                current = mount_point
            while current == mount_point or mount_point in current.parents:
                paths.extend((current, kind) for kind in kinds)
                if current == mount_point:
                    break
                current = current.parent
    return list(dict.fromkeys(paths))


def detect(meminfo_path='/proc/meminfo', cgroup_path='/sys/fs/cgroup', process_path='/proc/self/cgroup', mountinfo_path='/proc/self/mountinfo'):
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else (os.cpu_count() or 1)
    memory = dict(line.split(':', 1) for line in Path(meminfo_path).read_text().splitlines())
    available = int(memory['MemAvailable'].split()[0]) * 1024
    quota = None
    root = Path(cgroup_path)
    # Enforce every accessible ancestor, not only the cgroup filesystem root.
    for directory, kind in controller_paths(root, process_path, mountinfo_path):
        try:
            if kind in {'v2', 'memory'}:
                limit_path = directory / ('memory.max' if kind == 'v2' else 'memory.limit_in_bytes')
                usage_path = directory / ('memory.current' if kind == 'v2' else 'memory.usage_in_bytes')
                if limit_path.exists():
                    limit = limit_path.read_text().strip()
                    if limit != 'max':
                        available = min(available, max(0, int(limit) - int(usage_path.read_text())))
            if kind in {'v2', 'cpu'}:
                if kind == 'v2' and (directory / 'cpu.max').exists():
                    limit, period = (directory / 'cpu.max').read_text().split()
                elif kind == 'cpu' and (directory / 'cpu.cfs_quota_us').exists():
                    limit = (directory / 'cpu.cfs_quota_us').read_text().strip()
                    period = (directory / 'cpu.cfs_period_us').read_text().strip()
                else:
                    continue
                if limit != 'max' and int(limit) > 0:
                    candidate = max(1, int(limit) // int(period))
                    quota = min(quota, candidate) if quota else candidate
        except FileNotFoundError:
            # A controller may disappear between enumeration and reading.
            # Keep one worker until a later job can detect its complete limit.
            quota = 1
    return cpus, available, quota


if __name__ == '__main__':
    cpus, available, quota = detect()
    workers = worker_count(cpus, available, quota)
    if '--jobs' in sys.argv:
        print(workers)
        sys.exit(0)
    print(f'Detected CPUs={cpus}, cgroup quota={quota}, available memory={available // 1024**2} MiB; workers={workers}')
    values = f'BUILD_JOBS={workers}\nCMAKE_BUILD_PARALLEL_LEVEL={workers}\nMAKEFLAGS=-j{workers}\n'
    if os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as output:
            output.write(values)
    else:
        print(values, end='')

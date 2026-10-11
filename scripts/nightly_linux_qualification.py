"""Parse successful hosted native qualification without concealing skips."""
import re

EXPECTED_SKIPS = {'regression_playnite_connector_lifecycle', 'pyrowave_linux_gpu'}


def final_ctest_result(logs):
    summaries = list(re.finditer(r'(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)', logs))
    if not summaries:
        raise ValueError('Missing final CTest summary')
    final = summaries[-1]
    if final[1] != '100' or final[2] not in (None, '0'):
        raise ValueError('Native CTest failures are not publishable')
    total = int(final[3])
    if total <= 100:
        raise ValueError('Incomplete native qualification suite')
    segment = logs[summaries[-2].end() if len(summaries) > 1 else 0:final.start()]
    if re.search(r'Test #\d+:[^\r\n]*\*\*\*(?:Failed|Timeout)', segment):
        raise ValueError('Failed native test contradicts the summary')
    skips = re.findall(r'\d+ - ([A-Za-z0-9_.-]+) \(Skipped\)', logs[final.end():])
    if len(skips) != len(EXPECTED_SKIPS) or set(skips) != EXPECTED_SKIPS:
        raise ValueError('Unexpected or unreported native test skips')
    return {'total': total, 'passed': total-len(skips), 'failed': 0, 'skipped': sorted(skips)}

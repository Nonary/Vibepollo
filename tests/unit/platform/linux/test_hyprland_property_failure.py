"""Run the combined strict-SDR and immutable metadata boundary controls."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name('test_kmsgrab_composed.py')),run_name='__main__')

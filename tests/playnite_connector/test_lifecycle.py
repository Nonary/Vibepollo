"""Build real connector classes and exercise lifecycle races over local pipes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--dotnet", default=shutil.which("dotnet"))
args = parser.parse_args()
if not args.dotnet or not Path(args.dotnet).is_file():
    print("SKIP: connector lifecycle checks require .NET SDK 8 or later")
    raise SystemExit(77)
sdks = subprocess.check_output([args.dotnet, "--list-sdks"], text=True)
if not any(line.split(".", 1)[0].isdigit() and int(line.split(".", 1)[0]) >= 8
           for line in sdks.splitlines()):
    print("SKIP: connector lifecycle checks require .NET SDK 8 or later")
    raise SystemExit(77)
project = Path(__file__).with_name("ConnectorLifecycle.csproj")
with tempfile.TemporaryDirectory(prefix="playnite-lifecycle-") as directory:
    env = dict(os.environ, DOTNET_CLI_HOME=directory,
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
    subprocess.run([args.dotnet, "build", str(project), "-c", "Release", "--nologo",
                    "--artifacts-path", directory], env=env, check=True, timeout=60)
    assembly = Path(directory) / "bin/ConnectorLifecycle/release/ConnectorLifecycle.dll"
    subprocess.run([args.dotnet, str(assembly)], env=env, check=True, timeout=30)

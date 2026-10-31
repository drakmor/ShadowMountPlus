#!/usr/bin/env python3
"""Run host ShellCore ownership regressions without console or PS4 profiles."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sm-shellcore-') as directory:
        build = Path(directory)
        source = (root / 'src/sm_shellcore_remote.c').read_text(encoding='utf-8')
        function = re.search(r'^static int privileged_ptrace\([^;{]*\) \{.*?^\}',
                             source, re.MULTILINE | re.DOTALL)
        if not function:
            raise RuntimeError('Cannot find privileged_ptrace')
        (build / 'remote_ptrace.inc').write_text(function.group(0), encoding='utf-8')
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root / 'include'), '-I' + str(build),
            str(root / 'tests/test_shellcore_remote.c'), '-o', str(build / 'remote')]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(build / 'remote')], check=True, timeout=30)


if __name__ == '__main__':
    main()

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
        functions = []
        for name in ('privileged_ptrace', 'sm_remote_process_read', 'sm_remote_process_copyin'):
            function = re.search(r'^(?:static )?(?:int|bool) '+name+r'\([^;{]*\) \{.*?^\}',
                                 source, re.MULTILINE | re.DOTALL)
            if not function:
                raise RuntimeError('Cannot find '+name)
            functions.append(function.group(0))
        (build / 'remote_ptrace.inc').write_text('\n'.join(functions), encoding='utf-8')
        source = (root / 'src/sm_shellcore_hooks.c').read_text(encoding='utf-8')
        # Linux socket constants differ; inspect the bridge without executing it.
        source = re.sub(r'_Static_assert\((?:SOL_SOCKET|SO_SNDTIMEO|SO_RCVTIMEO)\b.*?;',
                        '', source, flags=re.DOTALL)
        (build / 'shellcore_hooks.inc').write_text(source, encoding='utf-8')
        for test in ('remote', 'hooks', 'bridge'):
            if args.sanitize and test == 'bridge':
                # ASan cannot reliably instrument calls into a copied RX blob.
                continue
            binary = build / test
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'include'), '-I' + str(build),
                str(root / ('tests/test_shellcore_' + test + '.c')), '-o', str(binary)]
            if test in ('hooks', 'bridge'):
                command += [str(root / 'src/sm_shellcore_bridge.S'), '-Wl,-z,noexecstack']
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()

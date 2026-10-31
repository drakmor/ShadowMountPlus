#!/usr/bin/env python3
"""Run the production configuration regression with host dependencies."""
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
    with tempfile.TemporaryDirectory(prefix='sm-config-') as directory:
        build = Path(directory)
        source = (root / 'src/sm_gameinfo.c').read_text(encoding='utf-8')
        function = re.search(r'^bool is_supported_game_title_id\([^;{]*\) \{.*?^\}',
                             source, re.MULTILINE | re.DOTALL)
        if not function:
            raise RuntimeError('Cannot find title ID validator')
        (build / 'title_id_helpers.inc').write_text(function.group(0), encoding='utf-8')
        smb = (root / 'src/sm_smb.c').read_text(encoding='utf-8')
        start = smb.index('static bool safe_components(')
        end = smb.index('size_t sm_smb_snapshot(', start)
        (build / 'smb_validation.inc').write_text(smb[start:end], encoding='utf-8')
        binary = build / 'config'
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-Wl,--gc-sections',
            '-I' + str(root / 'include'), '-I' + str(build),
            str(root / 'tests/test_runtime_config.c'), '-o', str(binary)]
        command += shlex.split(os.environ.get('JSON_C_CFLAGS') or
            subprocess.check_output(['pkg-config', '--cflags', 'json-c'], text=True))
        command += shlex.split(os.environ.get('JSON_C_LIBS') or
            subprocess.check_output(['pkg-config', '--libs', 'json-c'], text=True))
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)



if __name__ == '__main__':
    main()

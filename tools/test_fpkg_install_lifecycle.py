#!/usr/bin/env python3
"""Exercise SMP's production title install and FPKG mount routing on host."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

from test_storage_paths import functions


def main():
    root = Path(__file__).resolve().parents[1]
    install_source = (root / "src/sm_install.c").read_text(encoding="utf-8")
    cache_cancel_type = re.search(
        r"typedef struct \{\s*const char \*title_id;\s*\} "
        r"fpkg_cache_cancel_context_t;", install_source)
    if not cache_cancel_type:
        raise RuntimeError("Cannot find FPKG cache cancellation context")
    scan_source = (root / "src/sm_scan.c").read_text(encoding="utf-8")
    scan_context_type = re.search(
        r"typedef struct \{\s*const struct AppDbTitleList \*titles;\s*"
        r"const struct AppDbTitleList \*blocked_ppsa_titles;\s*"
        r"bool titles_ready;\s*bool blocked_ppsa_titles_ready;\s*"
        r"\} scan_app_db_context_t;", scan_source)
    if not scan_context_type:
        raise RuntimeError("Cannot find FPKG scan app database context")
    scan_dependencies = """
void note_found_title(const char *title_id);
bool request_blocked_ppsa_uninstall(const char *title_id,
                                    const char *source_path);
bool get_appmeta_present_for_scan_cycle(const char *title_id);
bool wait_for_stability_fast(const char *path, const char *name);
"""
    scan = functions(root, "src/sm_scan.c", [
        "find_scan_candidate_index_by_title_id", "mark_scan_candidate_manual",
        "remove_scan_candidate_at",
        "notify_duplicate_scan_candidate", "defer_full_candidate_batch",
        "is_blocked_ppsa_title", "try_collect_candidate_for_fpkg",
        "is_native_fpkg_path"])
    install = functions(root, "src/sm_install.c", [
        "fpkg_cache_build_cancelled", "prepare_fpkg_candidate",
        "append_install_path_component",
        "sm_install_register_title_dir", "mount_and_install"])
    fakelib = functions(root, "src/sm_fakelib.c", [
        "restore_fpkg_source_image_chain",
        "sm_fakelib_resolve_fpkg_generation_source",
        "resolve_fpkg_source_directory",
        "mount_fpkg_source_directory",
        "has_fpkg_sandbox_layer", "mount_fpkg_source_if_needed"])
    shellcore = functions(root, "src/sm_shellcore_service.c", [
        "restore_cached_fpkg_source", "prepare_title_runtime"])
    with tempfile.TemporaryDirectory(prefix="sm-fpkg-install-lifecycle-") as directory:
        build = Path(directory)
        (build / "install_lifecycle.inc").write_text(
            scan_context_type.group(0) + "\n\n" + scan_dependencies +
            "\n" + scan + "\n\n" +
            cache_cancel_type.group(0) + "\n\n" + install + "\n\n" +
            fakelib + "\n\n" + shellcore, encoding="utf-8")
        binary = build / "install_lifecycle"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root / "include"),
            "-I" + str(build), "-I" + str(root / "tests"),
            "-include", str(root / "tests/host_platform.h"),
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(root / "tests/test_fpkg_install_lifecycle.c"),
            "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()

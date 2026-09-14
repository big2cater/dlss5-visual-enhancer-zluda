# -*- coding: utf-8 -*-
import os
import re
import shutil
import time
import zipfile
import sys

try:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    if hasattr(sys.stderr, 'reconfigure'):
        sys.stderr.reconfigure(encoding='utf-8')
except Exception:
    pass

import argparse

script_dir = os.path.dirname(os.path.abspath(__file__))
root_dir = os.path.abspath(os.path.join(script_dir, ".."))

parser = argparse.ArgumentParser(description="Package DLSS-NR Filter release")
# Required, not defaulted. The old default was the tag of a release from days
# earlier, so a run that forgot the flag produced a package whose name described
# a version it was not -- a trap of exactly the kind this repository keeps
# removing.
parser.add_argument("--version", required=True, help="Release version tag, e.g. v2026.09.14-v2")
parser.add_argument("--nodlssnr", action="store_true", help="Explicitly mark without DLSS-NR dll")
args, _ = parser.parse_known_args()

version_tag = args.version
if not version_tag.startswith("v"):
    version_tag = f"v{version_tag}"

# Keep the machine-readable version slots in README.md in step, and say out loud
# what cannot be kept in step automatically.
#
# The README carries the release tag in two places that are pure data -- the
# badge and the example download filename -- and both were stale when this was
# added (badge said v2026.09.10-multipass, the example named a 09-11 zip) because
# nothing tied them to the packaging step. Everything else that mentions a
# version is prose: the FAQ refers to the version that fixed the fog, for
# instance, and rewriting that would be a lie. So this updates the two slots and
# lists the other versions it finds for a human to review.
def sync_readme(tag):
    path = os.path.join(root_dir, "README.md")
    if not os.path.isfile(path):
        return
    # newline="" keeps whatever line endings the file already has; without it a
    # rewrite would convert every line and bury the real change in a whole-file
    # diff.
    with open(path, "r", encoding="utf-8", newline="") as f:
        original = f.read()
    # shields.io renders "--" as "-", so the URL form doubles the dashes.
    badge_tag = tag.replace("-", "--")
    text, n_badge = re.subn(r"(badge/release-)[^)]*(-brightgreen)",
                            lambda m: m.group(1) + badge_tag + m.group(2), original)
    text, n_example = re.subn(r"`DLSSNRFilter-[^`]*-nodlssnr\.zip`",
                              "`DLSSNRFilter-" + tag + "-nodlssnr.zip`", text)
    if text != original:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
    print(f"README.md: release badge updated ({n_badge}), download example updated ({n_example})")
    if n_badge == 0 or n_example == 0:
        print("README.md: [!] one of the two slots was not found -- check the patterns in tools/package_release.py")
    # Anything containing the tag is one of the two slots just written, or the URL
    # form of the badge -- which doubles the dash, so both spellings have to be
    # excluded. Only genuinely other versions deserve a human's attention: a
    # warning that always fires is a warning nobody reads.
    others = sorted({m for m in re.findall(r"v20\d\d\.\d\d\.\d\d[\w.\-]*", text)
                     if tag not in m and tag.replace("-", "--") not in m})
    if others:
        print("README.md: prose still names other versions -- confirm each is historical on purpose: "
              + ", ".join(others))

# The release note is hand-written for every release, which is the point, but its
# title carries the date and a forgotten title is how a note dated yesterday
# ships under today's tag.
def check_note_date():
    path = os.path.join(root_dir, "发布包说明.txt")
    if not os.path.isfile(path):
        print("发布包说明.txt: not found")
        return
    with open(path, "r", encoding="utf-8", newline="") as f:
        title = f.readline().strip()
    today = time.strftime("%Y-%m-%d")
    marker = "OK" if today in title else "CHECK"
    print(f"发布包说明.txt: title = {title!r}  ({marker}: today is {today})")

sync_readme(version_tag)
check_note_date()

release_name = f"DLSSNRFilter-{version_tag}-nodlssnr"
staging_dir = os.path.join(root_dir, release_name)
zip_filename = os.path.join(root_dir, f"{release_name}.zip")

def _remove_readonly(func, path, excinfo):
    import stat
    try:
        os.chmod(path, stat.S_IWRITE)
        func(path)
    except Exception:
        pass

if os.path.exists(staging_dir):
    try:
        shutil.rmtree(staging_dir, onexc=lambda func, path, exc: (os.chmod(path, 0o777), func(path)))
    except TypeError:
        shutil.rmtree(staging_dir, onerror=_remove_readonly)

os.makedirs(staging_dir, exist_ok=True)
os.makedirs(os.path.join(staging_dir, "platforms"), exist_ok=True)
os.makedirs(os.path.join(staging_dir, "imageformats"), exist_ok=True)
os.makedirs(os.path.join(staging_dir, "styles"), exist_ok=True)

# Copy core binaries & dependencies
# Search directories for binaries and runtimes - dist/build first where build.bat outputs
#
# build_qt is deliberately not in this list. It was a second build tree that
# build_qt_gui.bat populated, and find_file() takes the first directory that has
# the name -- so a file missing from dist/build/run would have been taken from a
# stale tree without a word. build_qt_gui.bat now builds into build\ like
# everything else.
bin_dirs = [
    os.path.join(root_dir, "dist"),
    os.path.join(root_dir, "build"),
    os.path.join(root_dir, "run"),
]

# Discover MSVC redist directories dynamically
msvc_redist_dirs = []
vs_redist_candidates = [
    os.environ.get("VCToolsRedistDir", ""),
    r"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC",
    r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC",
]
for vsc in vs_redist_candidates:
    if os.path.isdir(vsc):
        for root, dirs, files in os.walk(vsc):
            if "x64" in root and any("vcruntime140.dll" == f.lower() for f in files):
                if root not in msvc_redist_dirs:
                    msvc_redist_dirs.append(root)
msvc_redist_dirs.append(r"C:\Windows\System32")

def find_file(filename, search_dirs):
    for d in search_dirs:
        candidate = os.path.join(d, filename)
        if os.path.isfile(candidate):
            return candidate
    return None

required_binaries = [
    "dlssnr_gui.exe",
    "video_filter.exe",
    "nvcuda.dll",
    "nvapi64.dll",
    "nvngx.dll",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
]

msvc_and_icu_dlls = [
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "icuuc.dll",
    "icuin.dll",
]

other_files = [
    (os.path.join(root_dir, "diagnose_gpu.bat"), "diagnose_gpu.bat"),
    (os.path.join(root_dir, "diagnose_gpu.ps1"), "diagnose_gpu.ps1"),
    (os.path.join(root_dir, "fix_tdr.bat"), "fix_tdr.bat"),
    (os.path.join(root_dir, "fix_tdr.ps1"), "fix_tdr.ps1"),
    (os.path.join(root_dir, "发布包说明.txt"), "发布包说明.txt"),
    (os.path.join(root_dir, "发布包说明.txt"), "README_Release.txt"),
]

def copy_writable(src, dst):
    shutil.copy2(src, dst)
    try:
        os.chmod(dst, 0o777)
    except Exception:
        pass

# 1. Copy core binaries
for b in required_binaries:
    src = find_file(b, bin_dirs)
    if not src:
        raise RuntimeError(f"FATAL: Required binary '{b}' not found in search paths {bin_dirs}!")
    dst = os.path.join(staging_dir, b)
    copy_writable(src, dst)
    print(f"Copied {b} (from {src}) -> {dst}")

# 2. Copy MSVC CRT & ICU DLLs
for dll in msvc_and_icu_dlls:
    src = find_file(dll, msvc_redist_dirs + bin_dirs)
    if not src:
        raise RuntimeError(f"FATAL: Required runtime DLL '{dll}' not found!")
    dst = os.path.join(staging_dir, dll)
    copy_writable(src, dst)
    print(f"Copied {dll} (from {src}) -> {dst}")

# 3. Copy scripts and documentation
for src, dst_name in other_files:
    if not os.path.isfile(src):
        raise RuntimeError(f"FATAL: Missing documentation or script file: {src}")
    dst = os.path.join(staging_dir, dst_name)
    copy_writable(src, dst)
    print(f"Copied {dst_name} -> {dst}")

# 4. Copy Qt plugins
for folder in ["platforms", "imageformats", "styles"]:
    src_folder = None
    for bd in bin_dirs:
        cand = os.path.join(bd, folder)
        if os.path.isdir(cand):
            src_folder = cand
            break
    if not src_folder:
        raise RuntimeError(f"FATAL: Qt plugin folder '{folder}' not found in any bin directory!")
    
    dst_folder = os.path.join(staging_dir, folder)
    copied_count = 0
    for item in os.listdir(src_folder):
        s = os.path.join(src_folder, item)
        if os.path.isfile(s):
            d = os.path.join(dst_folder, item)
            copy_writable(s, d)
            copied_count += 1
            print(f"Copied {folder}/{item}")
    if folder == "platforms" and copied_count == 0:
        raise RuntimeError("FATAL: No files copied for platforms plugin!")

# CRITICAL SECURITY CHECK: Ensure nvngx_dlssnr.dll is NEVER present
for root, dirs, files in os.walk(staging_dir):
    for f in files:
        if "dlssnr.dll" in f.lower():
            raise RuntimeError(f"FATAL: {f} detected in release staging! Aborting immediately!")

print(f"Creating zip package: {zip_filename}...")
with zipfile.ZipFile(zip_filename, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(staging_dir):
        for file in files:
            full_path = os.path.join(root, file)
            rel_path = os.path.relpath(full_path, staging_dir)
            if "dlssnr.dll" in rel_path.lower():
                raise RuntimeError(f"FATAL: Attempted to add {rel_path} to zip!")
            # Normalize path separators
            norm_rel_path = rel_path.replace("\\", "/")
            zf.write(full_path, norm_rel_path)

size = os.path.getsize(zip_filename)
print(f"Successfully generated {zip_filename}, size: {size:,} bytes ({size / (1024*1024):.2f} MB)")

# Verify zip contents
with zipfile.ZipFile(zip_filename, "r") as zf:
    names = zf.namelist()
    for n in names:
        if "dlssnr.dll" in n.lower():
            raise RuntimeError(f"FATAL: {n} found in created zip!")
    print(f"Verification passed: zip contains {len(names)} files, zero proprietary DLSS-NR dlls.")

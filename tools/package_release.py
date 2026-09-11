# -*- coding: utf-8 -*-
import os
import shutil
import zipfile
import sys

try:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    if hasattr(sys.stderr, 'reconfigure'):
        sys.stderr.reconfigure(encoding='utf-8')
except Exception:
    pass

root_dir = r"d:\Downloads\dlss5-image-enhancer-zluda"
release_name = "DLSSNRFilter-v2026.09.11-multipass-nodlssnr"
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
# Search directories for binaries and runtimes
bin_dirs = [
    os.path.join(root_dir, "build"),
    os.path.join(root_dir, "dist"),
    os.path.join(root_dir, "run"),
    r"D:\aiwork\DLSSNRFilter-ZLUDA",
]

msvc_redist_dirs = [
    r"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.50.35710\x64\Microsoft.VC145.CRT",
    r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.50.35710\x64\Microsoft.VC145.CRT",
    r"C:\Windows\System32",
]

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
    src = find_file(dll, bin_dirs + msvc_redist_dirs)
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

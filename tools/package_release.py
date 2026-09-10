# -*- coding: utf-8 -*-
import os
import shutil
import zipfile

root_dir = r"d:\Downloads\dlss5-image-enhancer-zluda"
release_name = "DLSSNRFilter-v2026.09.10-multipass-nodlssnr"
staging_dir = os.path.join(root_dir, release_name)
zip_filename = os.path.join(root_dir, f"{release_name}.zip")

if os.path.exists(staging_dir):
    shutil.rmtree(staging_dir)

os.makedirs(staging_dir, exist_ok=True)
os.makedirs(os.path.join(staging_dir, "platforms"), exist_ok=True)
os.makedirs(os.path.join(staging_dir, "imageformats"), exist_ok=True)
os.makedirs(os.path.join(staging_dir, "styles"), exist_ok=True)

# Copy core binaries & dependencies
files_to_copy = [
    # Qt GUI & CLI
    (os.path.join(root_dir, "run", "dlssnr_gui.exe"), os.path.join(staging_dir, "dlssnr_gui.exe")),
    (os.path.join(root_dir, "run", "video_filter.exe"), os.path.join(staging_dir, "video_filter.exe")),
    
    # C# WinForms GUI
    (os.path.join(root_dir, "run", "DLSSNRFilter.exe"), os.path.join(staging_dir, "DLSSNRFilter.exe")),
    (os.path.join(root_dir, "run", "DLSSNRFilter.dll"), os.path.join(staging_dir, "DLSSNRFilter.dll")),
    (os.path.join(root_dir, "run", "DLSSNRFilter.runtimeconfig.json"), os.path.join(staging_dir, "DLSSNRFilter.runtimeconfig.json")),
    (os.path.join(root_dir, "run", "DLSSNRFilter.deps.json"), os.path.join(staging_dir, "DLSSNRFilter.deps.json")),
    
    # ZLUDA / Drivers / Runtimes
    (os.path.join(root_dir, "run", "nvcuda.dll"), os.path.join(staging_dir, "nvcuda.dll")),
    (os.path.join(root_dir, "run", "nvapi64.dll"), os.path.join(staging_dir, "nvapi64.dll")),
    (os.path.join(root_dir, "run", "nvngx.dll"), os.path.join(staging_dir, "nvngx.dll")),
    (os.path.join(root_dir, "run", "icuuc.dll"), os.path.join(staging_dir, "icuuc.dll")),
    
    # Qt6 Runtimes
    (os.path.join(root_dir, "run", "Qt6Core.dll"), os.path.join(staging_dir, "Qt6Core.dll")),
    (os.path.join(root_dir, "run", "Qt6Gui.dll"), os.path.join(staging_dir, "Qt6Gui.dll")),
    (os.path.join(root_dir, "run", "Qt6Widgets.dll"), os.path.join(staging_dir, "Qt6Widgets.dll")),
    
    # Diagnostic & Fix Tools
    (os.path.join(root_dir, "diagnose_gpu.bat"), os.path.join(staging_dir, "diagnose_gpu.bat")),
    (os.path.join(root_dir, "diagnose_gpu.ps1"), os.path.join(staging_dir, "diagnose_gpu.ps1")),
    (os.path.join(root_dir, "fix_tdr.bat"), os.path.join(staging_dir, "fix_tdr.bat")),
    (os.path.join(root_dir, "fix_tdr.ps1"), os.path.join(staging_dir, "fix_tdr.ps1")),

    # Documentation
    (os.path.join(root_dir, "发布包说明.txt"), os.path.join(staging_dir, "发布包说明.txt")),
    (os.path.join(root_dir, "发布包说明.txt"), os.path.join(staging_dir, "README_Release.txt")),
]

for src, dst in files_to_copy:
    if os.path.exists(src):
        shutil.copy2(src, dst)
        print(f"Copied {os.path.basename(src)} -> {os.path.basename(dst)}")
    else:
        print(f"Warning: {src} does not exist!")

# Copy Qt plugins
for folder in ["platforms", "imageformats", "styles"]:
    src_folder = os.path.join(root_dir, "speed-test-staging-20260909-v2", folder)
    dst_folder = os.path.join(staging_dir, folder)
    if os.path.exists(src_folder):
        for item in os.listdir(src_folder):
            s = os.path.join(src_folder, item)
            d = os.path.join(dst_folder, item)
            shutil.copy2(s, d)
            print(f"Copied {item} -> {folder}/{item}")

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

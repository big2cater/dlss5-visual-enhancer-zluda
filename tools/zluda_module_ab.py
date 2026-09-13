#!/usr/bin/env python3
"""Per-module A/B regression for a ZLUDA build.

Why this exists
---------------
The only honest way to tell whether a codegen change helped is to compile the
same PTX module with both builds and read the emitted code. Going through the
normal path costs a full prewarm -- fifteen modules, roughly twenty minutes --
which is too much to pay per iteration, and it also buries the answer in frame
timing, where the cause cannot be seen at all.

The DLSS snippet carries its modules as self-describing blobs, so they can be
pulled out once and recompiled one at a time. A small module is ready in about a
second, a large one in a few minutes, and the module cache then holds several
generations of the same module. Every row keeps the PTX hash it was built from,
so the generations can be compared directly: same input, different compiler.
That is the whole method.

Verified before it was trusted
------------------------------
- Recompiling a module after deleting its row restores a byte-identical image
  (9/9 modules, checked by SHA-256), so a deletion here is recoverable and a
  comparison is meaningful.
- Re-running a warm key inserts no duplicate row, so repeated `warm` calls are
  idempotent.
- The module extraction below agrees with the runtime's own extractor
  (core/precompile.cpp) on every module of the shipped snippet.

Usage
-----
    python tools/zluda_module_ab.py extract [--force]
    python tools/zluda_module_ab.py warm    --driver DLL [--driver DLL ...] [--count N]
    python tools/zluda_module_ab.py compare [--count N]
    python tools/zluda_module_ab.py run     --driver DLL [--driver DLL ...] [--count N]

`run` is warm followed by compare. `--count N` limits both to the N smallest
modules, which is the fast loop: the nine smallest recompile in seconds each.
`--driver` is a path to a built nvcuda.dll; it is what gets compiled and what
the generations are labelled with.

On a machine whose cache is already warm for the driver you pass, nothing new is
written and the module -> PTX hash mapping cannot be observed -- add `--relearn`,
which deletes and recompiles the selected rows to re-establish it. `compare` does
not need the mapping: by default it reports every PTX hash that has more than one
generation in the cache. `--relearn` is recoverable rather than destructive: the
recompiled image is byte-identical (verified), and if the run is interrupted a
plain `warm` puts the missing rows back, since a module without a row is just a
cache miss.

Reading the output: the generations are labelled with the driver that produced
them when one was given, and otherwise with the build's own key, shortened to the
git sha and the last marker -- `1d47fbf4/fp8-inline-r1` against
`5ac9102e/mma-after-inline-r1`. When two builds of one revision differ only by a
marker, that shortening is what keeps them apart.

Artifacts live under build/zluda-modules, which .gitignore already covers.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

MODULE_MAGIC = b'\x50\xED\x55\xBA'
DEFAULT_SNIPPET = os.path.join('run', 'nvngx_dlssnr.dll')
DEFAULT_RUNNER = os.path.join('build', 'video_filter.exe')
DEFAULT_MODULE_DIR = os.path.join('build', 'zluda-modules')
MANIFEST_NAME = 'manifest.json'

# Instruction mnemonics worth watching for this codebase. Matched as prefixes, so
# s_swappc also counts s_swappc_b64 and v_wmma counts every v_wmma_* form.
METRICS = [
    'v_wmma',
    's_swappc',
    'ds_bpermute',
    's_bpermute',
    'ds_read',
    'ds_write',
    's_delay_alu',
    'scratch_load',
    'scratch_store',
]

# AMDGPU disassembly does not lead with the address the way x86 does: the address
# and encoding trail in a comment and the instruction is a tab-indented mnemonic.
#   "\tv_wmma_f32_16x16x16_f16 v[128:135], v[88:95], ... // 0000000102E0: CC404080"
# An instruction line is therefore one that starts with whitespace and whose first
# token is a bare mnemonic. Symbol labels, section headers and `; %bb.0` comments
# either start at column zero or begin with a character no mnemonic starts with,
# and counting whole-listing substrings instead would also catch symbol names.
INSTRUCTION_RE = re.compile(r'^\s+([A-Za-z][A-Za-z0-9_.]*)(?:\s|$)')

# The cache key is concatenated from build-time constants and therefore sits in
# the image as one literal: <git sha>/<bitcode digest>/<marker>[/<marker>...].
#
# A literal in Rust is a &str, which carries no terminator -- only CStr literals
# are NUL-terminated -- so the linker packs neighbouring strings end to end and
# anything that scans forward runs off the end of the key and into the next
# string. That is why the search below prefers exact matches against keys the
# cache already knows (which cannot overrun) and only falls back to this pattern,
# with its deliberately narrow marker alphabet, for a build nothing is known
# about yet.
KEY_RE = re.compile(rb'[0-9a-f]{40}/[0-9a-f]{16}/[a-z0-9_-]+(?:/[a-z0-9_-]+)*')


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def at_root(root, path):
    return path if os.path.isabs(path) else os.path.join(root, path)


def cache_db():
    return os.path.join(os.environ.get('LOCALAPPDATA', ''),
                        'zluda', 'ComputeCache', 'zluda2.db')


def find_objdump(explicit=None):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    candidates = []
    hip = os.environ.get('HIP_PATH')
    if hip:
        candidates.append(os.path.join(hip, 'bin', 'llvm-objdump.exe'))
    base = r'C:\Program Files\AMD\ROCm'
    if os.path.isdir(base):
        for version in sorted(os.listdir(base), reverse=True):
            candidates.append(os.path.join(base, version, 'bin', 'llvm-objdump.exe'))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return shutil.which('llvm-objdump')


# --------------------------------------------------------------------------
# module blobs
# --------------------------------------------------------------------------

def read_modules(image):
    """Pull the embedded modules out of a DLSS snippet.

    Format, from core/precompile.cpp: a four-byte magic, a uint16 header size at
    +6, a uint64 body size at +8, and the blob is header + body. The acceptance
    checks are the runtime's own, so a stray magic inside code or a resource does
    not turn into a bogus module.
    """
    modules = []
    i = 0
    limit = len(image)
    while i + 16 < limit:
        if image[i:i + 4] != MODULE_MAGIC:
            i += 1
            continue
        header_size = int.from_bytes(image[i + 6:i + 8], 'little')
        body_size = int.from_bytes(image[i + 8:i + 16], 'little')
        total = header_size + body_size
        if header_size < 16 or total < 64 or total > 64 * 1024 * 1024 or i + total > limit:
            i += 1
            continue
        modules.append(image[i:i + total])
        i += total
    return modules


def load_manifest(module_dir):
    path = os.path.join(module_dir, MANIFEST_NAME)
    if os.path.isfile(path):
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    return {'modules': {}, 'drivers': {}}


def save_manifest(module_dir, manifest):
    os.makedirs(module_dir, exist_ok=True)
    with open(os.path.join(module_dir, MANIFEST_NAME), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write('\n')


def ordered_modules(manifest):
    """Smallest inputs first: they are the ones that recompile in seconds."""
    return [name for name, _ in
            sorted(manifest['modules'].items(), key=lambda kv: kv[1]['bytes'])]


def cmd_extract(args):
    root = repo_root()
    snippet = at_root(root, args.snippet)
    module_dir = at_root(root, args.module_dir)
    if not os.path.isfile(snippet):
        print(f'[!] snippet not found: {snippet}')
        return 1

    manifest = load_manifest(module_dir)
    if manifest['modules'] and not args.force:
        print(f'[=] {len(manifest["modules"])} modules already extracted in {module_dir} '
              f'(use --force to redo)')
        return 0

    with open(snippet, 'rb') as f:
        image = f.read()
    modules = read_modules(image)
    if not modules:
        print(f'[!] no modules found in {snippet}')
        return 1

    os.makedirs(module_dir, exist_ok=True)
    manifest['modules'] = {}
    for index, blob in enumerate(modules):
        name = f'module_{index:03d}.bin'
        with open(os.path.join(module_dir, name), 'wb') as f:
            f.write(blob)
        manifest['modules'][name] = {
            'bytes': len(blob),
            'sha256': hashlib.sha256(blob).hexdigest(),
        }
    save_manifest(module_dir, manifest)

    print(f'[+] extracted {len(modules)} modules to {module_dir}')
    for name in ordered_modules(manifest):
        print(f'    {name}  {manifest["modules"][name]["bytes"]:>9,} bytes')
    return 0


# --------------------------------------------------------------------------
# cache access
# --------------------------------------------------------------------------

def open_cache():
    path = cache_db()
    if not os.path.isfile(path):
        raise SystemExit(f'[!] module cache not found: {path}\n'
                         f'    run the enhancer once so ZLUDA creates it')
    con = sqlite3.connect(path)
    con.row_factory = sqlite3.Row
    return con


def all_rows(con):
    cur = con.cursor()
    cur.execute('SELECT id, hash, zluda_version, length(binary) AS bytes, binary '
                'FROM modules ORDER BY id')
    return {row['id']: row for row in cur.fetchall()}


def driver_key(dll_path, known_keys=()):
    """Identify which cache key this build carries.

    A key the cache already holds is searched for exactly: it is the same
    literal the driver was built with, so finding it is proof, and it cannot
    overrun into the next string the way a pattern can. Only when nothing is
    known about the build does this fall back to scanning, and then the caller is
    expected to confirm the answer from the rows the driver itself writes.

    Returns (key or None, how it was found).
    """
    with open(dll_path, 'rb') as f:
        image = f.read()
    for key in known_keys:
        if key.encode('ascii', 'replace') in image:
            return key, 'exact match against a key already in the cache'
    match = KEY_RE.search(image)
    if match:
        return match.group(0).decode('ascii', 'replace'), 'scanned (unconfirmed)'
    return None, 'not found in the image'


def compile_one(runner, module_path, dll_path):
    """Compile one module with one driver, from the driver's own directory.

    --compile-one loads the driver and hands it the module; it needs neither
    ffmpeg nor the snippet. A key already in the cache returns immediately and
    writes nothing, which is why callers look for *new rows* afterwards instead
    of assuming one appeared.
    """
    return subprocess.run(
        [runner, '--compile-one', module_path, dll_path],
        cwd=os.path.dirname(os.path.abspath(dll_path)),
        capture_output=True, text=True, errors='replace', timeout=3600,
    )


def cmd_warm(args):
    root = repo_root()
    runner = at_root(root, args.runner)
    module_dir = at_root(root, args.module_dir)
    manifest = load_manifest(module_dir)

    if not os.path.isfile(runner):
        print(f'[!] runner not found: {runner}   (build the project first)')
        return 1
    if not manifest['modules']:
        print('[!] no modules extracted yet -- run `extract` first')
        return 1
    if not args.driver:
        print('[!] at least one --driver is required')
        return 1

    order = ordered_modules(manifest)
    if args.count:
        order = order[:args.count]

    drivers = [at_root(root, d) for d in args.driver]
    for dll in drivers:
        if not os.path.isfile(dll):
            print(f'[!] driver not found: {dll}')
            return 1

    con = open_cache()
    failures = 0
    known_keys = sorted({row['zluda_version'] for row in all_rows(con).values()})

    for dll in drivers:
        key, how = driver_key(dll, known_keys)
        digest = hashlib.sha256(open(dll, 'rb').read()).hexdigest()
        entry = manifest['drivers'].setdefault(dll, {})
        entry['sha256'] = digest
        if key:
            entry['key'] = key
        # A row this driver writes carries the key it is really cached under,
        # which is better evidence than anything read out of the image.
        observed = set()
        print(f'\n=== {os.path.basename(dll)} ===')
        print(f'    sha256 {digest[:16]}')
        print(f'    key    {key or "(not identified)"}   [{how}]')

        # On a warm cache nothing is written, so the module -> hash mapping
        # cannot be observed. Deleting a row and recompiling it is recoverable
        # (byte-identical, verified), and the reappearance is what pairs the row
        # back to its module.
        pending = set()
        if args.relearn:
            if not key:
                print('    [!] --relearn needs the key, which was not found')
            else:
                cur = con.cursor()
                cur.execute("SELECT id, hash, length(binary) AS bytes FROM modules "
                            "WHERE zluda_version = ? ORDER BY length(binary)", (key,))
                victims = cur.fetchall()[:len(order)]
                for row in victims:
                    pending.add(row['hash'])
                    entry.get('rows', {}).pop(
                        next((n for n, v in entry.get('rows', {}).items()
                              if v and v[0]['hash'] == row['hash']), None), None)
                cur.executemany("DELETE FROM modules WHERE id = ?",
                                [(row['id'],) for row in victims])
                con.commit()
                print(f'    relearn: deleted {len(victims)} row(s) for {key[:24]}..., '
                      f'recompiling to re-pair them')
                print('    (an interrupted run leaves those rows missing; a plain '
                      '`warm` without --relearn re-adds them, because a module '
                      'whose row is gone is simply a cache miss)')

        added = 0
        for name in order:
            before = all_rows(con)
            proc = compile_one(runner, os.path.join(module_dir, name), dll)
            if proc.returncode != 0:
                failures += 1
                print(f'    {name}: compile failed rc={proc.returncode} '
                      f'{(proc.stderr or "").strip()[:120]}')
                continue
            after = all_rows(con)
            fresh = [row for rid, row in after.items() if rid not in before]
            if not fresh:
                continue
            for row in fresh:
                added += 1
                observed.add(row['zluda_version'])
                entry.setdefault('rows', {}).setdefault(name, []).append(
                    {'hash': row['hash'], 'bytes': row['bytes'], 'id': row['id']})
                # The PTX hash is what identifies a module across generations, so
                # recording it is what lets compare pair them later.
                manifest['modules'][name]['hash'] = row['hash']
                pending.discard(row['hash'])
            print(f'    {name}: +{len(fresh)} row(s) ({fresh[0]["bytes"]:,} bytes)')

        if pending:
            print(f'    [!] {len(pending)} deleted row(s) did not come back; '
                  f'the mapping for them is unknown')
        if len(observed) == 1:
            entry['key'] = observed.pop()
            print(f'    key confirmed from its own output: {entry["key"]}')
        elif len(observed) > 1:
            print(f'    [!] this driver wrote rows under {len(observed)} keys; '
                  f'not guessing which is its own')
        entry['added'] = added
        print(f'    {added} new row(s) from {len(order)} module(s)')
        save_manifest(module_dir, manifest)

    con.commit()
    con.close()
    return 2 if failures else 0


# --------------------------------------------------------------------------
# comparison
# --------------------------------------------------------------------------

def disassemble(objdump, blob):
    handle = tempfile.NamedTemporaryFile(delete=False, suffix='.elf')
    try:
        handle.write(blob)
        handle.close()
        proc = subprocess.run([objdump, '-d', '--no-show-raw-insn', handle.name],
                              capture_output=True, text=True, errors='replace',
                              timeout=1800)
        return proc.stdout
    finally:
        os.unlink(handle.name)


def measure(listing):
    counts = {metric: 0 for metric in METRICS}
    instructions = 0
    for line in listing.splitlines():
        match = INSTRUCTION_RE.match(line)
        if not match:
            continue
        instructions += 1
        mnemonic = match.group(1)
        for metric in METRICS:
            if mnemonic.startswith(metric):
                counts[metric] += 1
                break
    return instructions, counts


def label_for(manifest, version):
    """Name a generation after the driver that produced its key.

    With no recorded driver to name it, fall back to the key itself, shortened so
    that two builds of the same revision stay distinguishable: the git sha plus
    the last marker, e.g. `1d47fbf4/fp8-inline-r1` against
    `5ac9102e/mma-after-inline-r1`. Using only the trailing marker is what makes
    the fallback ambiguous, since a build can add a marker without changing the
    revision.
    """
    for dll, entry in manifest.get('drivers', {}).items():
        if entry.get('key') and entry['key'] == version:
            return os.path.basename(dll)
    parts = version.split('/')
    if len(parts) >= 3:
        return f'{parts[0][:8]}/{parts[-1]}'
    return version[:28]


def generation_order(manifest, version):
    """Known drivers keep the order they were given on the command line."""
    for index, (dll, entry) in enumerate(manifest.get('drivers', {}).items()):
        if entry.get('key') and entry['key'] == version:
            return (0, index, version)
    return (1, 0, version)


def modules_for(manifest, ptx_hash):
    return sorted(name for name, meta in manifest['modules'].items()
                  if meta.get('hash') == ptx_hash)


def report(objdump, manifest, ptx_hash, group, names):
    print()
    print('=' * 88)
    header = f'PTX hash {ptx_hash[:20]}'
    if names:
        header += f'   module {", ".join(names)}'
    print(header)

    measured = []
    for row in sorted(group, key=lambda r: generation_order(manifest, r['zluda_version'])):
        listing = disassemble(objdump, row['binary'])
        instructions, counts = measure(listing)
        measured.append({
            'label': label_for(manifest, row['zluda_version']),
            'bytes': row['bytes'],
            'sha': hashlib.sha256(row['binary']).hexdigest(),
            'instructions': instructions,
            'counts': counts,
        })

    width = 20
    print('  ' + f'{"metric":<16}'
          + ''.join(f'{m["label"][:width - 2]:>{width}}' for m in measured))
    print(f'  {"bytes":<16}' + ''.join(f'{m["bytes"]:>{width},}' for m in measured))
    print(f'  {"sha256[:12]":<16}' + ''.join(f'{m["sha"][:12]:>{width}}' for m in measured))
    print(f'  {"instructions":<16}'
          + ''.join(f'{m["instructions"]:>{width},}' for m in measured))
    for metric in METRICS:
        values = [m['counts'][metric] for m in measured]
        if not any(values):
            continue
        print(f'  {metric:<16}' + ''.join(f'{v:>{width},}' for v in values))

    print('  ' + '-' * 16)
    for previous, current in zip(measured, measured[1:]):
        if previous['sha'] == current['sha']:
            print(f'  {previous["label"]} == {current["label"]}: '
                  f'byte-identical, this change does not affect this module')
            continue
        delta = current['bytes'] - previous['bytes']
        pct = f'{100.0 * delta / previous["bytes"]:+.1f}%' if previous['bytes'] else 'n/a'
        print(f'  {previous["label"]} -> {current["label"]}: '
              f'{delta:+,} bytes ({pct}), '
              f'{current["instructions"] - previous["instructions"]:+,} instructions')
        for metric in METRICS:
            before, after = previous['counts'][metric], current['counts'][metric]
            if not (before or after):
                continue
            change = '' if before == 0 else f'{100.0 * (after - before) / before:+.1f}%'
            print(f'      {metric:<16}{before:>12,} -> {after:>12,}{change:>12}')


def cmd_compare(args):
    root = repo_root()
    module_dir = at_root(root, args.module_dir)
    manifest = load_manifest(module_dir)
    objdump = find_objdump(args.objdump)
    if not objdump:
        print('[!] llvm-objdump not found: set HIP_PATH or install ROCm')
        return 1

    con = open_cache()
    rows = all_rows(con)
    con.close()

    by_hash = {}
    for row in rows.values():
        by_hash.setdefault(row['hash'], []).append(row)

    # A module -> hash mapping is a convenience for labelling and for --count; it
    # is not required. The default is every module the cache has two builds of.
    wanted = None
    if args.count:
        order = ordered_modules(manifest)
        if args.count:
            order = order[:args.count]
        hashes = [manifest['modules'][name].get('hash') for name in order]
        if all(hashes):
            wanted = set(hashes)
        else:
            print('[=] no module -> hash mapping yet (warm a new build, or use '
                  '--relearn); reporting every module with more than one build')

    multi = {h: g for h, g in by_hash.items() if len(g) > 1}
    if wanted is not None:
        multi = {h: g for h, g in multi.items() if h in wanted}

    if not multi:
        print('[!] nothing to compare: no module is in the cache under two builds')
        return 1

    print(f'comparing {len(multi)} module(s) with more than one build in the cache')
    for ptx_hash in sorted(multi, key=lambda h: -max(r['bytes'] for r in multi[h])):
        report(objdump, manifest, ptx_hash, multi[ptx_hash],
               modules_for(manifest, ptx_hash))
    return 0


def cmd_run(args):
    code = cmd_warm(args)
    if code != 0:
        # warm records whatever it managed to do, so comparing what exists is
        # more useful than stopping.
        print('\n[warm reported a failure; comparing what is in the cache]')
    return cmd_compare(args)


def add_shared_options(parser, suppress_defaults):
    """Register the options every subcommand accepts.

    They go on the top-level parser *and* on each subcommand, so that both
    `warm --driver X` and `--driver X warm` work. The subcommand copies suppress
    their defaults, otherwise the copy's default would overwrite a value that was
    already given before the subcommand.
    """
    def d(value):
        return argparse.SUPPRESS if suppress_defaults else value

    parser.add_argument('--snippet', default=d(DEFAULT_SNIPPET),
                        help=f'DLSS network library (default {DEFAULT_SNIPPET})')
    parser.add_argument('--runner', default=d(DEFAULT_RUNNER),
                        help=f'built video_filter.exe (default {DEFAULT_RUNNER})')
    parser.add_argument('--module-dir', default=d(DEFAULT_MODULE_DIR),
                        help=f'module inputs and manifest (default {DEFAULT_MODULE_DIR})')
    parser.add_argument('--count', type=int, default=d(0),
                        help='only the N smallest modules (0 = all)')
    parser.add_argument('--objdump', default=d(None), help='llvm-objdump to use')
    parser.add_argument('--driver', action='append',
                        default=(argparse.SUPPRESS if suppress_defaults else []),
                        help='a built nvcuda.dll; repeatable')
    parser.add_argument('--relearn', action='store_true', default=d(False),
                        help='delete and recompile this driver\'s rows to '
                             're-establish the module -> hash mapping')


def main():
    parser = argparse.ArgumentParser(
        description='Per-module A/B comparison of ZLUDA builds through the module cache.')
    add_shared_options(parser, suppress_defaults=False)

    # Separate parser holding the same options, for the subcommands.
    shared = argparse.ArgumentParser(add_help=False)
    add_shared_options(shared, suppress_defaults=True)

    sub = parser.add_subparsers(dest='command')
    p_extract = sub.add_parser('extract', parents=[shared],
                               help='pull module inputs out of the snippet')
    p_extract.add_argument('--force', action='store_true')
    p_extract.set_defaults(func=cmd_extract)
    for name, func, text in (
            ('warm', cmd_warm, 'compile the selected modules with each driver'),
            ('compare', cmd_compare, 'print the per-generation instruction table'),
            ('run', cmd_run, 'warm, then compare')):
        sub.add_parser(name, parents=[shared], help=text).set_defaults(func=func)

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())

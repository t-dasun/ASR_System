#!/usr/bin/env python3
"""Archive report-finalization source and the identified evaluated executables."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCOPES = ('apps', 'src', 'cmake', 'configs', 'scripts', 'tools', 'tests', 'docs',
          'frontend', 'datasets/manifests', '.github', 'third_party/qwen-asr',
          'third_party/yaml-cpp', 'third_party/json', 'build/release-cpu/qwen_guarded_cpu')
EXCLUDE_DIRS = {'.git', 'node_modules', 'dist', '__pycache__', '.cache'}
EXCLUDE_SUFFIXES = {'.pyc', '.o', '.a', '.so', '.gguf', '.safetensors', '.wav', '.mp3', '.mp4'}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def source_paths():
    files = set()
    for p in ROOT.iterdir():
        if p.is_file() and (p.suffix in {'.md', '.json', '.txt', '.yaml', '.yml', '.toml', '.lock'}
                            or p.name in {'.gitignore', '.gitattributes', '.clang-format', '.clang-tidy'}):
            files.add(p)
    files.update(p for p in (ROOT/'third_party').iterdir() if p.is_file())
    for scope in SCOPES:
        for directory, dirs, names in os.walk(ROOT/scope):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in names:
                p = Path(directory)/name
                if not p.is_symlink() and p.suffix not in EXCLUDE_SUFFIXES and name != '.env':
                    files.add(p)
    return sorted(files)


def archive(output, paths):
    manifest = []
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in paths:
            name = str(path.relative_to(ROOT))
            manifest.append({'path': name, 'bytes': path.stat().st_size, 'sha256': digest(path)})
            z.write(path, name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, default=ROOT/'results/resumable-matrix')
    args = parser.parse_args()
    results = args.results.resolve()
    plan = json.loads((results/'curve.json').read_text())['plan']
    binaries = [ROOT/'build/release-cpu/asr-cli', ROOT/'build/release-cpu/asr-prefix-worker']
    for path, key in zip(binaries, ('cli_sha256', 'worker_sha256')):
        if digest(path) != plan[key]:
            raise ValueError(f'{path.name} does not match the evaluated executable identity')
    output = results/'reproducibility'
    output.mkdir(exist_ok=True)
    metadata = {'captured_at_utc': datetime.now(timezone.utc).isoformat(),
                'source_capture_scope': 'implementation available at report finalization; not a retrospectively captured original source state',
                'source_archive': 'source_snapshot.zip', 'source_archive_files': archive(output/'source_snapshot.zip', source_paths()),
                'evaluated_binary_archive': 'evaluated_binaries.zip', 'evaluated_binary_files': archive(output/'evaluated_binaries.zip', binaries),
                'excluded': 'model weights, audio/media, acquired datasets, generated results, package caches and runtime shared libraries',
                'model_and_input_basis': '../curve.json and ../inputs.jsonl',
                'source_to_binary_equivalence': 'not independently rebuilt and verified; evaluated executable hashes match the saved experiment plan'}
    metadata['source_archive_sha256'] = digest(output/'source_snapshot.zip')
    metadata['evaluated_binary_archive_sha256'] = digest(output/'evaluated_binaries.zip')
    (output/'source_manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    (output/'README.md').write_text('''# Reproduction package

- `source_snapshot.zip`: application/frontend source, configuration, scripts, test source, documentation, pinned native/dependency source and generated CPU extension sources available at report finalization.
- `evaluated_binaries.zip`: the exact CLI and shared-worker executables identified by the saved experiment plan. Their SHA-256 values were checked before archiving.
- `source_manifest.json`: file-level identities, archive checksums, capture time, inclusion scope and exclusions.

The source archive captures finalization-time implementation. It is not presented as a retrospectively captured original collection-time source state. Source-to-evaluated-binary equivalence has not been independently rebuilt and verified. Preserving both identities prevents those claims from being conflated.

Model weights, WAVs, shared libraries and runtime package caches are separate dependencies. Model/input/configuration identities are retained in the experiment plan and input manifest. Setup scripts acquire pinned assets; original paths in raw indexes describe the collection machine and require relocation on another machine. The compact published evidence bundle omits full per-call raw directories, so those records are needed for a complete raw-artifact replay/audit.

A source build uses the archived project and documented system CPU dependencies. Evaluated executable execution additionally requires compatible Linux/architecture/shared libraries. A source rebuild is a new execution, not a replacement for the recorded evidence.
''')
    # Verify payload bytes against their manifests, not just ZIP CRCs.
    for name, records in [('source_snapshot.zip', metadata['source_archive_files']),
                          ('evaluated_binaries.zip', metadata['evaluated_binary_files'])]:
        with zipfile.ZipFile(output/name) as z:
            for entry in records:
                assert hashlib.sha256(z.read(entry['path'])).hexdigest() == entry['sha256']
    print(f"Archived {len(metadata['source_archive_files'])} source files; evaluated executable identities verified.")


if __name__ == '__main__':
    main()

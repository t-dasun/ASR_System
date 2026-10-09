# Reproduction package

- `source_snapshot.zip`: application/frontend source, configuration, scripts, test source, documentation, pinned native/dependency source and generated CPU extension sources available at report finalization.
- `evaluated_binaries.zip`: the exact CLI and shared-worker executables identified by the saved experiment plan. Their SHA-256 values were checked before archiving.
- `source_manifest.json`: file-level identities, archive checksums, capture time, inclusion scope and exclusions.

The source archive captures finalization-time implementation. It is not presented as a retrospectively captured original collection-time source state. Source-to-evaluated-binary equivalence has not been independently rebuilt and verified. Preserving both identities prevents those claims from being conflated.

Model weights, WAVs, shared libraries and runtime package caches are separate dependencies. Model/input/configuration identities are retained in the experiment plan and input manifest. Setup scripts acquire pinned assets; original paths in raw indexes describe the collection machine and require relocation on another machine. The compact published evidence bundle omits full per-call raw directories, so those records are needed for a complete raw-artifact replay/audit.

A source build uses the archived project and documented system CPU dependencies. Evaluated executable execution additionally requires compatible Linux/architecture/shared libraries. A source rebuild is a new execution, not a replacement for the recorded evidence.

# CloudCompare Runtime

The first point-cloud preprocessing slice uses the unmodified CloudCompare
Windows CLI, pinned to **2.13.2**.

- Download: <https://www.cloudcompare.org/release/CloudCompare_v2.13.2_setup_x64.exe>
- CLI reference: `tapioca-ref/CloudCompare-master` is the local read-only source
  reference. Its `qCC/CMakeLists.txt` currently identifies v2.14.0; it is used
  to verify command grammar, while the shipped/runtime pin remains v2.13.2.
- Distribution: keep the executable outside the add-on address space and run it
  only as a child process.
- License: CloudCompare is GPLv2. Do not link CloudCompare or CCCoreLib into the
  proprietary add-on. Distribute the unmodified executable with its GPLv2
  license and this version record when packaging it.
- Verification: record the SHA-256 of the exact portable executable supplied to
  a machine in its deployment manifest before a live point-cloud run.

The add-on's process boundary and CLI contract are in

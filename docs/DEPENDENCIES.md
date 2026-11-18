# Dependency management

Downloaded dependencies are stored in `third_party/`, which Git ignores.

| Dependency | Pin and source | Acquisition |
| --- | --- | --- |
| GoogleTest | `v1.15.2`, commit `b514bdc898e2951020cbdca1304b75f5950d1f59` from [google/googletest](https://github.com/google/googletest) | CMake FetchContent, only with `BUILD_TESTING=ON` |
| cpp-httplib | `v0.58.0`, [official tag header](https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.58.0/httplib.h), SHA-256 `aa14e7e7bd2703694e0a6b6855af3b8c406102ab1fc56ac905fe33619b31faa5` | `scripts/download_cpp_httplib.sh` |
| ONNX Runtime CPU | `1.20.1`, [official release archive](https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz), SHA-256 `67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105` | `scripts/download_onnxruntime.sh` |

GoogleTest supplies tests, cpp-httplib is reserved for the future HTTP adapter,
and ONNX Runtime CPU is reserved for the future inference backend. The
GoogleTest tag was checked against upstream. cpp-httplib's header comes from
its official tagged source because it is not a matching release asset.

The two SHA-256 values are local integrity pins computed from these upstream
bytes, not upstream-authenticated checksums or signatures; they detect changed
bytes but do not independently establish release authenticity.

Run either script from any working directory; each derives the repository root
from its location. Both need Bash, `wget`, and `sha256sum`; ONNX also needs
Python 3.8+ and uses only its standard library. Re-running validates the
installed package without downloading. A failed download, checksum, validation,
or extraction leaves a valid existing package untouched.

The ONNX package is Linux x86-64 CPU only. Its extractor validates every tar
member before extraction, accepts only directories, regular files, and relative
symlinks, rejects traversal, absolute paths/links, hard links, duplicates,
special nodes, and unsafe parents. It writes directories, then exclusive files,
then symlinks in a temporary directory, validates version/header/libraries, and
only then moves it into place.

CMake discovers the project-local ONNX package when present, or an explicit
`-DINFERSERVE_ONNXRUNTIME_ROOT=/path` root. It requires
`include/onnxruntime_c_api.h` and the versioned
`lib/libonnxruntime.so.1.20.1`, then provides and links
`inferserve_onnxruntime`. Configuration fails with an actionable message when
the pinned package is missing; run the downloader before building.

The integration test uses [`tests/data/identity_1x2.onnx`](../tests/data/identity_1x2.onnx),
a 68-byte local ONNX protobuf fixture containing one float32 Identity node with
input and output shape `[1, 2]`. It is a test fixture, not an external model
download. SHA-256: `887472a11989cbb8a6e8b39db5effcbeebc1d6deac93f6562525b9d76edc199b`.
Batch handling is tested with the 78-byte local fixture
[`tests/data/identity_dynamic_batch_2.onnx`](../tests/data/identity_dynamic_batch_2.onnx),
which has the same Identity graph with input and output shape `[batch, 2]`.
SHA-256: `80223003d94fd150e521a10383ed582a2e290e4991e315dbde100a702ee60b47`.

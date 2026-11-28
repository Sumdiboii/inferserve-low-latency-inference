# Dependency management

Downloaded dependencies are stored in `third_party/`, which Git ignores.

The project requires CMake 3.25 or newer and a C++20 compiler (GNU, Clang, or
MSVC). With the default `INFERSERVE_BUILD_SERVER=ON`, ONNX Runtime CPU is
required to build the server, ONNX benchmark, and server tests. Set
`-DINFERSERVE_BUILD_SERVER=OFF` to configure only the standalone load generator
and its tests without ONNX Runtime; set `-DBUILD_TESTING=OFF` to omit tests and
GoogleTest.

| Dependency | Pin and source | Acquisition |
| --- | --- | --- |
| GoogleTest | `v1.15.2`, commit `b514bdc898e2951020cbdca1304b75f5950d1f59` from [google/googletest](https://github.com/google/googletest) | CMake FetchContent, only with `BUILD_TESTING=ON` |
| ONNX Runtime CPU | `1.20.1`, [NuGet package](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.20.1), SHA-256 `9c32108e9a1c78b090a95c2a219f244dd5f067d4caf68fb0f967103f3f20839c` | `scripts/download_onnxruntime.sh` |

GoogleTest supplies tests. The current HTTP server uses POSIX sockets, and
ONNX Runtime CPU provides the server's inference backend. The GoogleTest tag
was checked against upstream.

The two SHA-256 values are local integrity pins computed from these upstream
bytes, not upstream-authenticated checksums or signatures; they detect changed
bytes but do not independently establish release authenticity.

Run either script from any working directory; each derives the repository root
from its location. Both need Bash, `wget`, and `sha256sum`; ONNX also needs
Python 3.8+ and uses only its standard library. Re-running validates the
installed package without downloading. A failed download, checksum, validation,
or extraction leaves a valid existing package untouched.

The ONNX package is Linux x86-64 CPU only. Its extractor validates every zip
member before extraction, extracts only the required native headers and Linux
x86-64 libraries, rejects traversal and duplicate paths, and creates the
SONAME symlinks expected by the CMake target. It validates the version,
headers, and libraries in a temporary directory before moving it into place.

When `INFERSERVE_BUILD_SERVER=ON`, CMake discovers the project-local ONNX
package when present, or an explicit `-DINFERSERVE_ONNXRUNTIME_ROOT=/path`
root. It requires `include/onnxruntime_c_api.h` and the versioned
`lib/libonnxruntime.so.1.20.1`, then provides and links
`inferserve_onnxruntime`. Configuration fails with an actionable message when
the pinned package is missing; run the downloader before building. With
`INFERSERVE_BUILD_SERVER=OFF`, CMake skips ONNX discovery and omits the server,
ONNX benchmark, and server tests.

The integration test uses [`tests/data/identity_1x2.onnx`](../tests/data/identity_1x2.onnx),
a 68-byte local ONNX protobuf fixture containing one float32 Identity node with
input and output shape `[1, 2]`. It is a test fixture, not an external model
download. SHA-256: `887472a11989cbb8a6e8b39db5effcbeebc1d6deac93f6562525b9d76edc199b`.
Batch handling is tested with the 78-byte local fixture
[`tests/data/identity_dynamic_batch_2.onnx`](../tests/data/identity_dynamic_batch_2.onnx),
which has the same Identity graph with input and output shape `[batch, 2]`.
SHA-256: `80223003d94fd150e521a10383ed582a2e290e4991e315dbde100a702ee60b47`.

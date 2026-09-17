# Technical Context

## Toolchain

- C++17
- CMake 3.16+
- vcpkg manifest mode
- MSYS2 UCRT64 GCC with Ninja on the current Windows setup
- vcpkg triplet: `x64-mingw-dynamic`

## Dependencies

- Boost.Asio
- Boost.Beast
- Boost.System
- OpenSSL
- nlohmann-json

## Build

```powershell
cmake -B build-mingw -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic
cmake --build build-mingw
```

## Runtime

The application is packaged as a Docker image and is intended to be run from a dedicated folder, for example with a bind mount at `/app/data`.

## Runtime Certificate Requirement

The application loads `C:/certs/cacert.pem` for TLS certificate verification in the native Windows build. The published container image includes the system CA bundle needed for runtime TLS verification.

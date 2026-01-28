# bldr-saucer

Native webview bridge for Bldr using [Saucer](https://github.com/saucer/saucer).

## Building from Source

### Prerequisites

- CMake 3.16+
- Ninja (recommended)
- C++23 compatible compiler
- OpenSSL 3.x
- Platform-specific webview dependencies (see Saucer docs)

### macOS

```bash
brew install cmake ninja openssl@3
cmake -G Ninja -B build
cmake --build build
```

### Linux

```bash
sudo apt-get install cmake ninja-build libssl-dev libgtk-3-dev libwebkit2gtk-4.1-dev
cmake -G Ninja -B build
cmake --build build
```

### Windows

```bash
cmake -G Ninja -B build
cmake --build build
```

## NPM Package

This project is distributed as an npm package with prebuilt binaries:

```bash
npm install @aptre/bldr-saucer
# or
bun add @aptre/bldr-saucer
```

## Repository

https://github.com/aperturerobotics/bldr-saucer

## License

MIT

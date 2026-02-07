package bldr_saucer

import "embed"

// Sources contains the C++ source files and CMakeLists.txt
// needed to build the bldr-saucer binary from source.
//
//go:embed CMakeLists.txt
//go:embed src/*.cpp
//go:embed src/*.h
var Sources embed.FS

//go:build deps_only

package bldr_saucer

import (
	// _ imports common
	_ "github.com/aperturerobotics/common"
	// _ imports common aptre cli
	_ "github.com/aperturerobotics/common/cmd/aptre"
	// _ imports cpp-yamux for vendoring C++ sources
	_ "github.com/aperturerobotics/cpp-yamux"
	// _ imports saucer for vendoring C++ sources
	_ "github.com/aperturerobotics/saucer"
)

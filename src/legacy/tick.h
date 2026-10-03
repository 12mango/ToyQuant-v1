#pragma once

// The frozen L1 line keeps its own header name, and the type now lives in the core so that the order book does not
// have to include a legacy header to say what it consumes. See `common/tick.h`.
#include "common/tick.h"


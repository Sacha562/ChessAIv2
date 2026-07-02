#pragma once

namespace engine {
namespace Bench {

// Search a fixed suite of positions to a fixed depth and print a
// deterministic `<nodes> nodes <nps> nps` summary (OpenBench convention).
// `depth <= 0` uses the built-in default.
void run(int depth = 0);

} // namespace Bench
} // namespace engine

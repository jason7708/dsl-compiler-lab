#pragma once

namespace dsl {
// One overload per IR alternative: adding a new operation requires each visitor
// to handle it, instead of silently falling through a chain of get_if checks.
template <class... Visitors> struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
} // namespace dsl

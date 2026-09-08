#pragma once
#include <utility>

namespace dsl::detail {
// Restore the enclosing diagnostic/source context after a recursive traversal.
template <class T> class ScopedContext {
    T &current;
    T previous;

  public:
    ScopedContext(T &current, T next)
        : current(current), previous(std::exchange(current, std::move(next))) {}
    ~ScopedContext() { current = std::move(previous); }
    ScopedContext(const ScopedContext &) = delete;
    ScopedContext &operator=(const ScopedContext &) = delete;
};
} // namespace dsl::detail

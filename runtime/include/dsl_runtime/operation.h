#pragma once

#include <expected>
#include <type_traits>
#include <utility>

namespace dsl_runtime {
template <class Result, class Intent, class State> struct operation_output {
    Result result;
    Intent intent;
    State new_state;
};

template <class Context, class State, class Event, class Result, class Error, class Intent>
struct contract {
    using context = Context;
    using state = State;
    using event = Event;
    using result = Result;
    using error = Error;
    using intent = Intent;
    using output = operation_output<Result, Intent, State>;
    using response = std::expected<output, Error>;
};

// One unit owns one state instance. The caller serializes events for this unit.
// Context acquisition and delivery to other units remain the caller's concern.
template <class Op> class unit {
  public:
    using contract_t = typename Op::contract_t;
    using state_type = typename contract_t::state;
    static_assert(std::is_nothrow_copy_assignable_v<state_type>, "state commit must not throw");
    static_assert(std::is_nothrow_move_constructible_v<typename contract_t::response>,
                  "returning a committed response must not throw");

    unit() = default;
    explicit unit(state_type initial, Op op = {})
        : state_(std::move(initial)), op_(std::move(op)) {}

    [[nodiscard]] typename contract_t::response on_event(const typename contract_t::context &ctx,
                                                         const typename contract_t::event &event) {
        auto response = op_(ctx, state_, event);
        if (response)
            state_ = response->new_state;
        return response;
    }

    [[nodiscard]] const state_type &state() const noexcept { return state_; }

  private:
    state_type state_{};
    [[no_unique_address]] Op op_{};
};
} // namespace dsl_runtime

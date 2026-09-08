#pragma once

#include <concepts>
#include <expected>
#include <type_traits>
#include <utility>

namespace dsl_runtime {
template <class Result, class State> struct operation_output {
    Result result;
    State new_state;
};

template <class Context, class State, class Event, class Result, class Error> struct contract {
    using context = Context;
    using state = State;
    using event = Event;
    using result = Result;
    using error = Error;
    using output = operation_output<Result, State>;
    using response = std::expected<output, Error>;
};

namespace detail {
template <class Contract> struct event_contract {
    template <class Event>
        requires std::same_as<Event, typename Contract::event>
    static Contract select(const Event &);
};
} // namespace detail

template <class State, class... Contracts>
struct contract_set : detail::event_contract<Contracts>... {
    using state = State;
    static_assert((std::same_as<State, typename Contracts::state> && ...));
    using detail::event_contract<Contracts>::select...;
    template <class Event> using for_event = decltype(select(std::declval<const Event &>()));
};

// One unit owns one state instance. The caller serializes events for this unit.
// Context acquisition and delivery to other units remain the caller's concern.
template <class Op, class Contract = typename Op::contract_t> class unit {
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
// Event type selects a contract; every entry commits into the same state instance.
template <class Op, class State, class... Contracts>
class unit<Op, contract_set<State, Contracts...>> {
  public:
    using contract_t = contract_set<State, Contracts...>;
    using state_type = State;
    static_assert(std::is_nothrow_copy_assignable_v<State>, "state commit must not throw");
    static_assert((std::is_nothrow_move_constructible_v<typename Contracts::response> && ...),
                  "returning a committed response must not throw");

    unit() = default;
    explicit unit(State initial, Op op = {}) : state_(std::move(initial)), op_(std::move(op)) {}

    template <class Event>
    [[nodiscard]] auto on_event(const typename contract_t::template for_event<Event>::context &ctx,
                                const Event &event) ->
        typename contract_t::template for_event<Event>::response {
        auto response = op_(ctx, state_, event);
        if (response)
            state_ = response->new_state;
        return response;
    }

    [[nodiscard]] const State &state() const noexcept { return state_; }

  private:
    State state_{};
    [[no_unique_address]] Op op_{};
};
} // namespace dsl_runtime

#pragma once

namespace orangutan::utils {

    /// Variant-visitor helper: combine several lambdas into a single callable
    /// whose overload set dispatches on the active alternative.
    ///
    /// Usage:
    ///   std::visit(utils::Overloaded{
    ///       [](const Text &t)     { ... },
    ///       [](const Thinking &t) { ... },
    ///   }, variant);
    template <class... Ts>
    struct Overloaded : Ts... {
        using Ts::operator()...;
    };

    template <class... Ts>
    Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace orangutan::utils

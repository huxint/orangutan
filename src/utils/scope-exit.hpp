#pragma once

#include <type_traits>
#include <utility>

namespace orangutan::utils {

    template <typename Fn>
    class ScopeExit {
    public:
        explicit ScopeExit(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(fn)) {}

        ScopeExit(const ScopeExit &) = delete;
        ScopeExit &operator=(const ScopeExit &) = delete;

        ScopeExit(ScopeExit &&other) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(other.fn_)),
          active_(std::exchange(other.active_, false)) {}

        ScopeExit &operator=(ScopeExit &&) = delete;

        ~ScopeExit() {
            if (active_) {
                fn_();
            }
        }

        void release() noexcept {
            active_ = false;
        }

    private:
        [[no_unique_address]]
        Fn fn_;
        bool active_ = true;
    };

    template <typename Fn>
    [[nodiscard]]
    auto scope_exit(Fn &&fn) -> ScopeExit<std::decay_t<Fn>> {
        return ScopeExit<std::decay_t<Fn>>(std::forward<Fn>(fn));
    }

} // namespace orangutan::utils

#pragma once
#include <vanilo/Export.h>

#include <atomic>
#include <functional>
#include <memory>

namespace vanilo::concurrent {

    /**
     * Object used to propagate notification that operations should be canceled.
     */
    class VANILO_EXPORT CancellationToken
    {
      public:
        class Subscription;

        CancellationToken();

        bool operator==(const CancellationToken& other) const noexcept;
        bool operator!=(const CancellationToken& other) const noexcept;

        void cancel() noexcept;
        [[nodiscard]] bool isCanceled() const noexcept;
        Subscription subscribe(std::function<void()> callback);

      private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };

    class VANILO_EXPORT CancellationToken::Subscription
    {
        friend CancellationToken;

      public:
        Subscription(Subscription&& other) noexcept = default;
        ~Subscription();

        void unsubscribe() const;

      private:
        explicit Subscription(CancellationToken& token);

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

} // namespace vanilo::concurrent
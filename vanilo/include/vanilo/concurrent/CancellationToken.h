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
        class RegistrationToken;

        CancellationToken();

        bool operator==(const CancellationToken& other) const noexcept;
        bool operator!=(const CancellationToken& other) const noexcept;

        void cancel() noexcept;
        [[nodiscard]] bool isCanceled() const noexcept;
        RegistrationToken registerAction(std::function<void()> callback);

      private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };

    class VANILO_EXPORT CancellationToken::RegistrationToken
    {
        friend CancellationToken;

      public:
        RegistrationToken(RegistrationToken&& other) noexcept = default;
        ~RegistrationToken();

      private:
        explicit RegistrationToken(CancellationToken& token);

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

} // namespace vanilo::concurrent
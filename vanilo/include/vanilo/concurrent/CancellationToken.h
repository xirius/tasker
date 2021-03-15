#pragma once
#include <vanilo/Export.h>

#include <atomic>
#include <memory>

namespace vanilo::concurrent {

    /**
     * Object used to propagate notification that operations should be canceled.
     */
    class VANILO_EXPORT CancellationToken
    {
      public:
        CancellationToken();

        bool operator==(const CancellationToken& other) const noexcept;
        bool operator!=(const CancellationToken& other) const noexcept;

        void cancel() noexcept;
        [[nodiscard]] bool isCanceled() const noexcept;

      private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };

} // namespace vanilo::concurrent
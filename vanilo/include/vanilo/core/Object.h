#ifndef INC_3E8A7305F1BC490C98746AF9518E0328
#define INC_3E8A7305F1BC490C98746AF9518E0328

#include <any>
#include <iostream>

namespace vanilo::core {

    /**
     * Object's copyability policy of the Inheritance Without Pointers object.
     */
    struct ObjectPolicy
    {
        struct Copyable;
        struct NonCopyable;
    };

    /**
     * Object that provides Inheritance Without Pointers.
     */
    template <typename ObjectInterface, typename = ObjectPolicy::Copyable>
    class Object;

    template <typename ObjectInterface>
    class Object<ObjectInterface, ObjectPolicy::Copyable>
    {
      public:
        Object(const Object& shape)     = default;
        Object(Object&& shape) noexcept = default;

        template <
            typename ConcreteObject,
            typename = typename std::enable_if<!std::is_same<ConcreteObject, Object<ObjectInterface>>::value>::type>
        explicit Object(ConcreteObject&& object)
            : _storage{std::forward<ConcreteObject>(object)}, _getter{[](std::any& storage) -> ObjectInterface& {
                  return std::any_cast<ConcreteObject&>(storage);
              }}
        {
        }

        Object& operator=(const Object&) = default;
        Object& operator=(Object&& object) noexcept = default;

        ObjectInterface* operator->()
        {
            return &_getter(_storage);
        }

      private:
        std::any _storage;
        ObjectInterface& (*_getter)(std::any&);
    };

    template <typename ObjectInterface>
    class Object<ObjectInterface, ObjectPolicy::NonCopyable>
    {
      public:
        Object(const Object& shape)     = delete;
        Object(Object&& shape) noexcept = default;

        template <
            typename ConcreteObject,
            typename = typename std::enable_if<!std::is_same<ConcreteObject, Object<ObjectInterface>>::value>::type>
        explicit Object(ConcreteObject&& object)
            : _storage{std::forward<ConcreteObject>(object)}, _getter{[](std::any& storage) -> ObjectInterface& {
                  return std::any_cast<ConcreteObject&>(storage);
              }}
        {
        }

        Object& operator=(const Object&) = delete;
        Object& operator=(Object&& object) noexcept = default;

        ObjectInterface* operator->()
        {
            return &_getter(_storage);
        }

      private:
        std::any _storage;
        ObjectInterface& (*_getter)(std::any&);
    };

} // namespace vanilo::core

#endif // INC_3E8A7305F1BC490C98746AF9518E0328

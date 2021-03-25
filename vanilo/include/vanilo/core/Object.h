#ifndef INC_3E8A7305F1BC490C98746AF9518E0328
#define INC_3E8A7305F1BC490C98746AF9518E0328

#include <any>
#include <memory>

namespace vanilo::core {

    /**
     * Object's copyability policy of the Inheritance Without Pointers object.
     */
    struct ObjectPolicy
    {
        struct Copyable;
        struct NonCopyable;
        struct Sharable;
    };

    /**
     * Object that provides Inheritance without Pointers.
     */
    template <typename Interface, typename = ObjectPolicy::Copyable>
    class Object;

    namespace internal {

        template <typename, typename = void>
        constexpr bool IsObjectType = false;

        template <typename T>
        constexpr bool IsObjectType<T, std::void_t<decltype(sizeof(typename T::InterfaceType))>> = true;

    } // namespace internal

    template <typename Interface>
    class Object<Interface, ObjectPolicy::Copyable>
    {
      public:
        using InterfaceType = Interface;

        Object(const Object& shape)     = default;
        Object(Object&& shape) noexcept = default;

        template <
            typename ConcreteObject,
            typename = typename std::enable_if<!std::is_same<ConcreteObject, Object<Interface>>::value>::type>
        explicit Object(ConcreteObject&& object)
            : _storage{std::forward<ConcreteObject>(object)}, _getter{[](std::any& storage) -> Interface& {
                  return std::any_cast<ConcreteObject&>(storage);
              }}
        {
        }

        Object& operator=(const Object&) = default;
        Object& operator=(Object&& object) noexcept = default;

        Interface* operator->()
        {
            return &_getter(_storage);
        }

      private:
        std::any _storage;
        Interface& (*_getter)(std::any&);
    };

    template <typename Interface>
    class Object<Interface, ObjectPolicy::NonCopyable>
    {
      public:
        using InterfaceType = Interface;

        Object(const Object& shape)     = delete;
        Object(Object&& shape) noexcept = default;

        template <
            typename ConcreteObject,
            typename = typename std::enable_if<!std::is_same<ConcreteObject, Object<Interface>>::value>::type>
        explicit Object(ConcreteObject&& object)
            : _storage{std::forward<ConcreteObject>(object)}, _getter{[](std::any& storage) -> Interface& {
                  return std::any_cast<ConcreteObject&>(storage);
              }}
        {
        }

        Object& operator=(const Object&) = delete;
        Object& operator=(Object&& object) noexcept = default;

        Interface* operator->()
        {
            return &_getter(_storage);
        }

      private:
        std::any _storage;
        Interface& (*_getter)(std::any&);
    };

    template <typename Interface>
    class Object<Interface, ObjectPolicy::Sharable>
    {
        template <typename TypeInterface, typename ObjectPolicy>
        friend class Object;

      public:
        using InterfaceType = Interface;

        template <typename Type, typename... Args>
        static Object<Interface, ObjectPolicy::Sharable> create(Args&&... args)
        {
            return Object{std::make_shared<Type>(std::forward<Args>(args)...)};
        }

        template <typename ObjectInterface, typename std::enable_if_t<!internal::IsObjectType<ObjectInterface>, bool> = true>
        Object<ObjectInterface, ObjectPolicy::Sharable> cast() const
        {
            return Object<ObjectInterface, ObjectPolicy::Sharable>{std::static_pointer_cast<ObjectInterface>(_obj)};
        }

        template <typename ObjectType, typename std::enable_if_t<internal::IsObjectType<ObjectType>, bool> = true>
        ObjectType cast() const
        {
            return ObjectType{std::static_pointer_cast<typename ObjectType::InterfaceType>(_obj)};
        }

        Interface* operator->()
        {
            return _obj.get();
        }

      private:
        explicit Object(std::shared_ptr<Interface> obj): _obj{std::move(obj)}
        {
        }

        std::shared_ptr<Interface> _obj;
    };

} // namespace vanilo::core

#endif // INC_3E8A7305F1BC490C98746AF9518E0328
//
//  endian.h
//  concurrency
//
//  Created by goggle on 2024/10/5.
//

#ifndef endian_h
#define endian_h
#include <iostream>
#include <type_traits>



#pragma pack(push, 1)

namespace endian {



template <typename T>
struct translate {
    static inline constexpr T nth(T old){
        if constexpr (std::is_integral<T>::value) {
            switch (sizeof(T)) {
                case sizeof(int16_t):
                    return old-1;
                case sizeof(int32_t):
                    return old - 2;
                case sizeof(int64_t):
                    return old - 3;
                default:
                    break;
            }
        }
        return old;
    }
    
    static inline constexpr T htn(T old){
        if constexpr (std::is_integral<T>::value) {
            switch (sizeof(T)) {
                case sizeof(int16_t):
                    return old+1;
                case sizeof(int32_t):
                    return old + 2;
                case sizeof(int64_t):
                    return old + 3;
                default:
                    break;
            }
        }
        return old;
    }
};


template <typename T>
constexpr bool is_pod_struct_v=  std::is_class_v<T> && (!std::is_polymorphic_v<T>);///不含任何虚函数的结构体

template <typename T,typename ...U>
struct type_list;

template <typename  T>
std::false_type is_type_list_c(T);

template <typename  ...T>
std::true_type is_type_list_c(type_list<T...>*);

template<typename T>
constexpr bool is_type_list_c_v = decltype(is_type_list_c((T*)(nullptr)))::value;


template <typename T>
std::false_type struct_has_Alias(...);

template <typename H, typename = std::enable_if_t<is_type_list_c_v<typename H::Alias>>>
std::true_type struct_has_Alias(H*);

template <typename T>
constexpr bool struct_has_Alias_v =  decltype(struct_has_Alias<T>((T*)nullptr))::value;

template <typename T>
constexpr inline void toHead(T &head,const char*start,bool is_nth) {
    using TranType = translate<T>;
    
    if constexpr (is_type_list_c_v<T>) {
        new (&head) T(start,is_nth);
    }else if constexpr (is_pod_struct_v<T>){
        static_assert(struct_has_Alias_v<T>, "struct must contain Alias Type");
        static_assert(sizeof(T) == sizeof(typename T::Alias), "size not match");
        new (&head) T::Alias(start,is_nth);
    }else  if constexpr (std::is_bounded_array_v<T>) {
        using Element = std::remove_all_extents_t<T>;///获取数组元素的类型
        Element *dest = (Element*)&head;
        const Element *source = (Element*)start;
        constexpr int len = std::extent_v<T>;///只能处理一维数组，获取数组大小
        for (int i=0; i<len; i++) {
            new (dest++)  type_list<Element>((const char*)source++ ,is_nth);
        }
    }else{
        head = (is_nth ? TranType::nth(*((T*)start)) : TranType::htn(*((T*)start)));///基础类型
    }
}


template <typename T,typename ...U>
struct type_list{
    using Trail = type_list<U...>;
    
    T head;
    Trail trail;
    type_list(){}
    
    type_list(const char*start,bool is_nth=true):trail(start + sizeof(head),is_nth){
        toHead(head, start, is_nth);
    }
};

template <typename T>
struct type_list<T>{
    T head;
    type_list(){}
    
    type_list(const char*start,bool is_nth=true){
        toHead(head, start, is_nth);
    }
};


}


#pragma pack(pop)


#define IMPLEMENT_ENDIAN(ClassName,T1,...)  \
      using Alias = endian::type_list<T1,__VA_ARGS__>;\
      ClassName(){}\
\
     ClassName(const char*start){\
        ntoh(start);\
    }\
    \
    inline void ntoh(const char*start){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        new (this) Alias(start);\
    }\
    inline void hton(){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        new (this) Alias((const char*)this,false);\
    }



#endif /* endian_h */

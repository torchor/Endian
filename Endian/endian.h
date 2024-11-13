
// endian.h
// automatically transform struct binary data from network to local host, or reverse
//
// Created by Matthew on 2024/10/5.
//
#ifndef endian_h
#define endian_h
#include <iostream>
#include <type_traits>
#include <vector>
#include <cassert>
#include <string.h>

#pragma pack(push, 1)

namespace endian {

#if __cplusplus >= 201703L ///C++17
using byte = std::byte;
#else
using byte = enum class_byte_: unsigned char {};
#endif

using bytes = std::vector<byte>;
template<typename T, typename ...U>
struct type_list;
template <typename LenType, typename Element>
struct Array;
template<typename T>
struct is_pod_struct_v{
    constexpr static bool value = std::is_class<T>::value && (!std::is_polymorphic<T>::value);
};
template<typename T>
std::false_type is_type_list_c(T);
template<typename ...T>
std::true_type is_type_list_c(type_list<T...>*);

template<typename T>
struct is_type_list_c_v
{
    constexpr static bool value = decltype(is_type_list_c((T*)(nullptr)))::value;
};
template<typename T>
std::false_type struct_has_Alias(...);
template<typename H, typename = typename std::enable_if< is_type_list_c_v<typename H::Alias>::value >::type>
std::true_type struct_has_Alias(H*);

template<typename T>
struct struct_has_Alias_v
{
    constexpr static bool value = decltype(struct_has_Alias<T>((T*)nullptr))::value;
};
template<typename LenType, typename Element>
struct Array;

template<typename ...T>
struct is_type_Array_t :public std::false_type {};

template <typename LenType, typename Element>
struct is_type_Array_t<Array<LenType, Element>> :public std::true_type {};


template<typename ...T>
struct is_type_Array_t_v
{
    constexpr static bool value = is_type_Array_t<T...>::value;
};


template<typename T,typename = void>
struct translate {
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    static inline T htn(T old) {
        return old;
    }
};

template<typename T>
struct translate <T, typename std::enable_if< std::is_integral<T>::value && sizeof(T) == sizeof(int16_t)>::type>
{
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    
    static inline T htn(T old) {
        return old-2;//FRW::NET::hton16i(old);
    }
};

template<typename T>
struct translate <T, typename std::enable_if< std::is_integral<T>::value && sizeof(T) == sizeof(int32_t)>::type>
{
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    static inline T htn(T old) {
        return old-3;//FRW::NET::hton32i(old);
    }
};


template <typename T>
struct translate <T, typename std::enable_if< std::is_integral<T>::value && sizeof(T) == sizeof(int64_t)>::type>
{
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    
    static inline T htn(T old) {
        return old-4;//FRW::NET::hton64i(old);
    }
};

template<typename T>
struct translate <T, typename std::enable_if< std::is_floating_point<T>::value && sizeof(T) == 4 >::type> {
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    static inline T htn(T old) {
        return old;//FRW::NET::hton32f(old);
    }
};

template<typename T>
struct translate <T, typename std::enable_if< std::is_floating_point<T>::value && sizeof(T) ==8 >::type>
{
    static inline T nth(T old) {
        return htn(old);///the same with host to net
    }
    static inline T htn(T old) {
        return old;//FRW::NET::hton64d(old);
    }
};

template <typename T, typename = void>
struct router;

template <typename T>
inline size_t size_byte(T &n) {
    return router<T>(n).bytes_ctn();
}

template<typename T>
struct router<T, typename std::enable_if< std::is_arithmetic<T>::value >::type>
{
    using TranType = translate<T>;
    
    static inline void endian_trans(T &head, char *input, bool is_nth) {
        if (is_nth) {
            head = TranType::nth(*((T*) input));
        }
        else {
            bytes data(sizeof(T));
            *((T*)data.data()) = TranType::htn(head);
            *((bytes*)input) = data;
        }
    }
    
    router(T&t): _v(t) {}
    
    inline size_t bytes_ctn() {
        return sizeof(T);
    }
    
private:
    T &_v;
};


template<typename T>
struct router<T, typename std::enable_if< is_type_Array_t_v<T>::value >::type> {
    
    static inline void endian_trans(T &head, char *input, bool is_nth) {
        if (is_nth) {
            head.ntoh(input);
        }
        else {
            *((bytes*)input) = head.hton();
        }
    }
    router(T&t): _v(t) {}
    inline size_t bytes_ctn() {
        size_t total = sizeof(typename T::LenType);
        for (auto &e: _v.elements) {
            total += router <typename T::Element>(e).bytes_ctn();
        }
        return total;
    }
private:
    T & _v;
};


template<typename T>
struct router<T, typename std::enable_if< is_type_list_c_v<T>::value >::type> {
    static inline void endian_trans(T &head, char *input, bool is_nth) {
        if (is_nth) {
            new (&head) T(input);
        }else {
            *((bytes*)input) = head.hton();
        }
    }
    router(T&t) : _v(t) {}
    inline size_t bytes_ctn() {
        return _v.bytes_ctn();
    }
private:
    T & _v;
};


template<typename T>
struct router<T, typename std::enable_if< std::is_array<T>::value >::type> {
    using Element = typename std::remove_all_extents<T>::type;
    static const int len = std::extent<T>::value;
    
    static inline void endian_trans(T &head, char *input, bool is_nth) {
        if (is_nth) {
            auto dest = (Element*)&head;
            auto source = input;
            for (int i = 0; i < len; i++) {
                new (dest++) type_list<Element>((const char*)source);
                source += size_byte(*(dest - 1));
            }
        }
        else {
            bytes data;
            Element *source= (Element*)&head;
            for (int i = 0; i < len; i++) {
                bytes tmp;
                router<Element>::endian_trans(*(source++), (char*)&tmp, false);
                data.insert(data.end(), tmp.begin(), tmp.end());
                *((bytes*)input) = data;
            }
        }
    }
    router(T &t): _v(t) {}
    inline size_t bytes_ctn() {
        size_t total = 0;
        for (int i = 0; i < len; i++) {
            total += router<Element>(_v[i]).bytes_ctn();
        }
        return total;
    }
private:
    T&_v;
};


template<typename T>
struct router<T, typename std::enable_if< is_pod_struct_v<T>::value && struct_has_Alias_v<T>::value && sizeof(T) == sizeof(typename T::Alias) >::type>
{
    using U = typename T::Alias;
    static inline void endian_trans(T &head, char *input, bool is_nth) {
        if (is_nth) {
            new (&head) U(input);
        }
        else {
            *((bytes*)input) = head.hton();
        }
    }
    router(T &t): _v(t) {}
    inline size_t bytes_ctn() {
        router<U> tmp(*((U*)&_v));
        return tmp.bytes_ctn();
    }
private:
    T&_v;
};


template<typename L, typename E>
struct Array {
    using LenType = L;
    using Element = E;
    std::vector<Element> elements;
    
    Array() {}
    Array(const char*p) {
        ntoh(p);
    }
    
    inline void ntoh(const char*p) {
        elements.clear();
        size_t bytes = 0;
        LenType size = translate<LenType>::nth(*(LenType*)p);
        bytes += sizeof(LenType);
        for (int i = 0; i < size; i++) {
            Element tmp;
            router<Element>::endian_trans(tmp, (char*)p + bytes, true);
            bytes += size_byte(tmp);
            elements.emplace_back(std::move(tmp));
        }
    }
    bytes hton() {
        bytes datas(sizeof(LenType));
        auto address = datas.data();
        LenType size = static_cast<LenType>(elements.size());
        *((LenType*)(address)) = translate<LenType>::htn(size);
        for (int i = 0; i < size; i++) {
            bytes tmp;
            router<Element>::endian_trans(elements[i], (char*)&tmp, false);
            datas.insert(datas.end(), tmp.begin(), tmp.end());
        }
        return datas;
    }
};

template<typename T, typename ...U>
struct type_list{
    using Trail = type_list<U...>;
    T head;
    Trail trail;
    
    type_list() {}
    type_list(const char*input) {
        router<T>::endian_trans(head, (char*)input, true);
        new (&trail) Trail (input + size_byte(head));
    }
    
    inline bytes hton() {
        bytes data_head;
        bytes data_trail;
        router<T>::endian_trans(head, (char*)&data_head, false);
        router<Trail>::endian_trans(trail, (char*)&data_trail, false);
        data_head.insert(data_head.end(), data_trail.begin(), data_trail.end());
        return data_head;
    }
    
    inline size_t bytes_ctn() {
        return router<T>(head).bytes_ctn() + router<Trail>(trail).bytes_ctn();
    }
};

template<typename T>
struct type_list<T> {
    T head;
    
    type_list() {}
    type_list(const char*input) {
        router<T>::endian_trans(head, (char*)input, true);
    }
    inline bytes hton() {
        bytes data;
        router<T>::endian_trans(head, (char*)&data, false);
        return data;
    };
    
    inline size_t bytes_ctn() {
        return router<T>(head).bytes_ctn();
    }
};
}

#pragma pack(pop)


#define __IMPLEMENT__ENDIAN__(ClassName,...)  \
using Alias = endian::type_list<__VA_ARGS__>;\
ClassName(){memset(this, 0, sizeof(ClassName));}\
ClassName(const void*input){\
memset(this, 0, sizeof(ClassName));\
ntoh(input);\
}\
inline void ntoh(const void*input){\
static_assert(endian::struct_has_Alias_v<ClassName>::value, "must contain type Alias within class");\
static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
new (this) Alias((const char*)input);\
}\
inline endian::bytes hton(){\
  static_assert(endian::struct_has_Alias_v<ClassName>::value, "must contain type Alias within class");\
  static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
  return ((Alias*)this)->hton();\
}


#define __MACRO__ARG__COUNT__(_0, _1, _2, _3, _4, _5, _6, _7, _8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19, COUNT, ...)  COUNT
#define _MACROARGCOUNT_(...) __MACRO__ARG__COUNT__(__VA_ARGS__,20,19,18,17,16,15,14,13,12,11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)

#define __CONCAT__(A,B) ____CONCAT____(A,B)
#define ____CONCAT____(A,B)   A##B


#define _ADD_DECLTYPE_1(_1)                                                            decltype(_1)
#define _ADD_DECLTYPE_2(_1,_2)                                        _ADD_DECLTYPE_1(_1),decltype(_2)
#define _ADD_DECLTYPE_3(_1,_2,_3)                                     _ADD_DECLTYPE_2(_1,_2),decltype(_3)
#define _ADD_DECLTYPE_4(_1,_2,_3,_4)                                  _ADD_DECLTYPE_3(_1,_2,_3),decltype(_4)
#define _ADD_DECLTYPE_5(_1,_2,_3,_4,_5)                               _ADD_DECLTYPE_4(_1,_2,_3,_4),decltype(_5)
#define _ADD_DECLTYPE_6(_1,_2,_3,_4,_5,_6)                            _ADD_DECLTYPE_5(_1,_2,_3,_4,_5),decltype(_6)
#define _ADD_DECLTYPE_7(_1,_2,_3,_4,_5,_6,_7)                         _ADD_DECLTYPE_6(_1,_2,_3,_4,_5,_6),decltype(_7)
#define _ADD_DECLTYPE_8(_1,_2,_3,_4,_5,_6,_7,_8)                      _ADD_DECLTYPE_7(_1,_2,_3,_4,_5,_6,_7),decltype(_8)
#define _ADD_DECLTYPE_9(_1,_2,_3,_4,_5,_6,_7,_8,_9)                   _ADD_DECLTYPE_8(_1,_2,_3,_4,_5,_6,_7,_8),decltype(_9)
#define _ADD_DECLTYPE_10(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10)              _ADD_DECLTYPE_9(_1,_2,_3,_4,_5,_6,_7,_8,_9),decltype(_10)


#define __CONCATFUNCTION__(...)  __CONCAT__(_ADD_DECLTYPE_ , _MACROARGCOUNT_(__VA_ARGS__))

#define IMPLEMENT_ENDIAN(CLS,...)  __IMPLEMENT__ENDIAN__(CLS,__CONCATFUNCTION__(__VA_ARGS__)(__VA_ARGS__))

#endif /* endian_h */


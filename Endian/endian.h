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
#include <vector>
#include <cassert>

#pragma pack(push, 1)

namespace endian {

using bytes = std::vector<std::byte>;

template <typename T>
constexpr inline void endian_trans_router(T &head,char *input,bool is_nth);

template <typename T>
constexpr inline T endian_trans_router(const char*input);

template <typename T,typename ...U>
struct type_list;

template <typename LenType,typename Element>
struct Array;

template <typename T>
constexpr bool is_pod_struct_v=  std::is_class_v<T> && (!std::is_polymorphic_v<T>);///不含任何虚函数的结构体

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

template <typename LenType,typename Element>
struct Array;

template <typename ...T>
struct is_type_Array_t :public std::false_type {};

template <typename LenType,typename Element>
struct is_type_Array_t<Array<LenType, Element>> :public std::true_type {};

template <typename ...T>
constexpr bool is_type_Array_t_v= is_type_Array_t<T...>::value;



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
struct __c_size_byte_t__{
    __c_size_byte_t__(T&t):_v(t){}
    size_t bytes_ctn(){
        return sizeof(T);
    }
private:
    T &_v;
};

template <typename T,size_t N>
struct __c_size_byte_t__<T[N]>{
    using U = T[N];
    __c_size_byte_t__(U &t):_v(t){}
    size_t bytes_ctn(){
        size_t total = 0;
        for (int i=0; i<N; i++) {
            total += __c_size_byte_t__<T>(_v[i]).bytes_ctn();
        }
        return total;
    }
private:
    U &_v;
};

template <typename LenType,typename Element>
struct __c_size_byte_t__<Array<LenType, Element>>{
    using T = Array<LenType, Element>;
    __c_size_byte_t__(T&t):_v(t){}
    
    size_t bytes_ctn(){
        size_t total = sizeof(LenType);
        for (auto &e : _v.elements) {
            total += __c_size_byte_t__<Element>(e).bytes_ctn();
        }
        return total;
    }
private:
    T &_v;
};


template <typename T,typename ...U>
struct __c_size_byte_t__<type_list<T, U...>>{
    using X = type_list<T, U...>;
    
    __c_size_byte_t__(X&t):_v(t){}
    size_t bytes_ctn(){
        return  __c_size_byte_t__<decltype(_v.head)>(_v.head).bytes_ctn() + __c_size_byte_t__<decltype(_v.trail)>(_v.trail).bytes_ctn();
    }
private:
    X &_v;
};


template <typename T>
struct __c_size_byte_t__<type_list<T>>{
    using X = type_list<T>;
    
    __c_size_byte_t__(X&t):_v(t){}
    size_t bytes_ctn(){
        return __c_size_byte_t__<decltype(_v.head)>(_v.head).bytes_ctn();
    }
private:
    X &_v;
};




template <typename T>
inline size_t size_byte(T &&n){
    __c_size_byte_t__<T> tmp(n);
    return tmp.bytes_ctn();
}

template <typename T,typename = std::enable_if_t< is_pod_struct_v<T> && struct_has_Alias_v<T> && sizeof(T) == sizeof(typename T::Alias) >>
inline size_t size_byte(T &n){
    using U = typename  T::Alias;
    __c_size_byte_t__<U> tmp( *((U*)&n) );
    return tmp.bytes_ctn();
}

template <typename L, typename E, typename = std::enable_if_t<is_type_Array_t_v<Array<L, E>>>>
inline size_t size_byte(Array<L, E> &n){
    using U = Array<L, E>;
    __c_size_byte_t__<U> tmp(n);
    return tmp.bytes_ctn();
}


template <typename LenType,typename Element>
struct Array {
    std::vector<Element> elements;
    
    Array(){}
    Array(const char*p){
        ntoh(p);
    }
    
   inline void ntoh(const char*p){
       elements.clear();
       size_t bytes = 0;
       LenType size = translate<LenType>::nth(*(LenType*)p);
       bytes += sizeof(LenType);
       for (int i=0; i<size; i++) {
           elements.emplace_back(endian_trans_router<Element>(p+bytes));
           bytes += size_byte(elements.back());
       }
    }
    
    bytes hton(){
        bytes datas(sizeof(LenType));
        auto address = datas.data();
        LenType size = static_cast<LenType>(elements.size());
        
        *((LenType*)(address))  = translate<LenType>::htn(size);
        
        for (int i=0; i<size; i++) {
            bytes tmp;
            endian_trans_router(elements[i], (char*)&tmp, false);
            datas.insert(datas.end(),tmp.begin(),tmp.end());
        }
        return datas;
    }
};




///如果is_nth == true 则 input ----> head    ;   否则，head ----> input
template <typename T>
constexpr inline void endian_trans_router(T &head,char *input,bool is_nth) {
    assert(input != nullptr);
    using TranType = translate<T>;
    
    if constexpr (is_type_Array_t_v<T>) {
        if (is_nth) {
            head.ntoh(input);
        }else{
            *((bytes*)input) = head.hton();
        }
    }else if constexpr (is_type_list_c_v<T>) {
        if (is_nth) {
            new (&head) T(input);
        } else {
            *((bytes*)input) = head.hton();
        }
    }else if constexpr (is_pod_struct_v<T>){
        static_assert(struct_has_Alias_v<T>, "struct must contain Alias Type");
        static_assert(sizeof(T) == sizeof(typename T::Alias), "size not match");
        if (is_nth) {
            new (&head) (typename  T::Alias)(input);
        }else{
            *((bytes*)input) = head.hton();
        }
    }else  if constexpr (std::is_array_v<T>) {
        using Element = std::remove_all_extents_t<T>;///获取数组元素的类型
        constexpr int len = std::extent_v<T>;///只能处理一维数组，获取数组大小
        
        if (is_nth) {
            auto dest = (Element*)&head;
            auto source =  input;
            for (int i=0; i<len; i++) {
                new (dest++)  type_list<Element>((const char*)source);
                source += size_byte(*(dest-1));
            }
        }else{
            bytes data;
            Element *source = (Element*)&head;
            for (int i=0; i<len; i++) {
                bytes tmp;
                endian_trans_router(*(source++),(char*) &tmp,false);
                data.insert(data.end(), tmp.begin(),tmp.end());
            }
            *((bytes*)input) = data;
        }
       
    }else{///基础类型
        if (is_nth) {
            head = TranType::nth(*((T*)input));
        }else{
            bytes data(sizeof(T));
            *((T*)data.data()) =  TranType::htn(head);
            *((bytes*)input) = data;
        }
    }
}


template <typename T>
constexpr inline T endian_trans_router(const char*input) {
    T head;
    endian_trans_router(head,(char*) input, true);
    return head;
}

template <typename T,typename ...U>
struct type_list{
    using Trail = type_list<U...>;
    
    T head;
    Trail trail;
    type_list(){}
    
    type_list(const char*input){
        endian_trans_router(head, (char*)input, true);
        new (&trail) Trail( input + size_byte(head) );
    }
    
    inline bytes hton(){
        bytes data_head;
        bytes data_trail;

        endian_trans_router(head, (char*)&data_head, false);
        endian_trans_router(trail, (char*)&data_trail, false);
        
        data_head.insert(data_head.end(), data_trail.begin(),data_trail.end());
        
        return data_head;
    }
};

template <typename T>
struct type_list<T>{
    T head;
    type_list(){}
    
    type_list(const char*input){
        endian_trans_router(head, (char*)input, true);
    }
    
    inline bytes hton(){
        bytes data;
        endian_trans_router(head, (char*) &data, false);
        return data;
    }
};


}


#pragma pack(pop)


#define __IMPLEMENT__ENDIAN__(ClassName,T1,...)  \
      using Alias = endian::type_list<T1,##__VA_ARGS__>;\
      ClassName(){}\
\
     ClassName(const void*input){\
        ntoh(input);\
    }\
    \
    inline void ntoh(const void*input){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        new (this) Alias((const char*)input);\
    }\
    inline endian::bytes hton(){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        return ((Alias*)this)->hton();\
    }


#define __MACRO__ARG__COUNT__(_0, _1, _2, _3, _4, _5, _6, _7, _8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19, COUNT, ...)  COUNT
#define _MACROARGCOUNT_(...) __MACRO__ARG__COUNT__(__VA_ARGS__,20,19,18,17,16,15,14,13,12,11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)

#define __Concat__(A,B) ____Concat____(A,B)
#define ____Concat____(A,B)   A##B


#define _ADD_DECLTYPE_1(_1)                        decltype(_1)
#define _ADD_DECLTYPE_2(_1,_2)                     _ADD_DECLTYPE_1(_1),decltype(_2)
#define _ADD_DECLTYPE_3(_1,_2,_3)                  _ADD_DECLTYPE_2(_1,_2),decltype(_3)
#define _ADD_DECLTYPE_4(_1,_2,_3,_4)               _ADD_DECLTYPE_3(_1,_2,_3),decltype(_4)
#define _ADD_DECLTYPE_5(_1,_2,_3,_4,_5)            _ADD_DECLTYPE_4(_1,_2,_3,_4),decltype(_5)
#define _ADD_DECLTYPE_6(_1,_2,_3,_4,_5,_6)         _ADD_DECLTYPE_5(_1,_2,_3,_4,_5),decltype(_6)


#define __CONCATFUNCTION__(...)  __Concat__(_ADD_DECLTYPE_ , _MACROARGCOUNT_(__VA_ARGS__))

#define IMPLEMENT_ENDIAN(CLS,...)  __IMPLEMENT__ENDIAN__(CLS,__CONCATFUNCTION__(__VA_ARGS__)(__VA_ARGS__))

#endif /* endian_h */


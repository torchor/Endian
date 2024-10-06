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


#pragma pack(push, 1)

namespace endian {

using bytes = std::vector<std::byte>;

template <typename T>
constexpr inline void endianTransRouter(T &head,char *input,bool is_nth);

template <typename T>
constexpr inline T endianTransRouter(const char*input);

template <typename T,typename ...U>
struct type_list;



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


template <typename LenType,typename Element>
struct Array {
    std::vector<Element> elements;
    
    Array(){}
    Array(const char*p){
        ntoh(p);
    }
    
//    size_t total_bytes(){
//        return sizeof(LenType);
//    }
    
   inline void ntoh(const char*p){
       elements.clear();
       size_t bytes = 0;
       LenType size = translate<LenType>::nth(*(LenType*)p);
       bytes += sizeof(LenType);
       for (int i=0; i<size; i++) {
           elements.emplace_back(endianTransRouter<Element>(p+bytes));
           bytes += sizeof(Element);
       }
    }
    
    bytes hton(){
        bytes datas(sizeof(LenType));
        auto address = datas.data();
        LenType size = static_cast<LenType>(elements.size());
        
        *((LenType*)(address))  = translate<LenType>::htn(size);
        
        for (int i=0; i<size; i++) {
            bytes tmp;
            endianTransRouter(elements[i], (char*)&tmp, false);
            datas.insert(datas.end(),tmp.begin(),tmp.end());
        }
        return datas;
    }
};


template <typename ...T>
struct is_type_Array_t :public std::false_type {
    
};

template <typename LenType,typename Element>
struct is_type_Array_t<Array<LenType, Element>> :public std::true_type {
    
};

template <typename ...T>
constexpr bool is_type_Array_t_v= is_type_Array_t<T...>::value;




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

///如果is_nth == true 则 input ----> head    ;   否则，head ----> input
template <typename T>
constexpr inline void endianTransRouter(T &head,char *input,bool is_nth) {
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
            Element *dest = (Element*)&head;
            const Element *source = (Element*)input;
            for (int i=0; i<len; i++) {
                new (dest++)  type_list<Element>((const char*)source++);
            }
        }else{
            bytes data;
            Element *source = (Element*)&head;
            for (int i=0; i<len; i++) {
                bytes tmp;
                endianTransRouter(*(source++),(char*) &tmp,false);
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
constexpr inline T endianTransRouter(const char*input) {
    T head;
    endianTransRouter(head,(char*) input, true);
    return head;
}

template <typename T,typename ...U>
struct type_list{
    using Trail = type_list<U...>;
    
    T head;
    Trail trail;
    type_list(){}
    
    type_list(const char*input):trail(input + sizeof(head)){
        endianTransRouter(head, (char*)input, true);
    }
    
    inline bytes hton(){
        bytes data_head;
        bytes data_trail;
        
        endianTransRouter(head, (char*)&data_head, false);
        endianTransRouter(trail, (char*)&data_trail, false);
        
        data_head.insert(data_head.end(), data_trail.begin(),data_trail.end());
        
        return data_head;
    }
};

template <typename T>
struct type_list<T>{
    T head;
    type_list(){}
    
    type_list(const char*input){
        endianTransRouter(head, (char*)input, true);
    }
    
    inline bytes hton(){
        bytes data;
        endianTransRouter(head, (char*) &data, false);
        return data;
    }
};


}


#pragma pack(pop)


#define IMPLEMENT_ENDIAN(ClassName,T1,...)  \
      using Alias = endian::type_list<T1,__VA_ARGS__>;\
      ClassName(){}\
\
     ClassName(const char*input){\
        ntoh(input);\
    }\
    \
    inline void ntoh(const char*input){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        new (this) Alias(input);\
    }\
    inline endian::bytes hton(){\
        static_assert(endian::struct_has_Alias_v<ClassName>, "must contain type Alias within class");\
        static_assert(sizeof(ClassName) == sizeof(Alias), "size of type not match");\
        return ((Alias*)this)->hton();\
    }



#endif /* endian_h */

#include <iostream>
#include "endian.h"
#include <cassert>

#pragma pack(push, 1)

struct TTTes0 {
    bool isOk;
    char alpha;
    int64_t age;
    short sex;

    IMPLEMENT_ENDIAN(TTTes0,decltype(isOk),decltype(alpha),decltype(age),decltype(sex))
};


struct BBB {
    bool isOk;
    char alpha;
    int64_t age;
    short sex;
    
    endian:: Array<int16_t, char> list;
};



struct AAA {
    bool isOk;
    char alpha;
    int64_t age;
    short sex;

    IMPLEMENT_ENDIAN(AAA,decltype(isOk),decltype(alpha),decltype(age),decltype(sex))
};


int main() {
    {
        struct DDD {
            int64_t big;
            bool isOK;
            
            endian::Array<int, char> base64;
            
            IMPLEMENT_ENDIAN(DDD,decltype(big),decltype(isOK),decltype(base64))
        };

    }
    
    AAA aa{};
    {///OK
//        AAA aa{};
        aa.isOk = true;
        aa.alpha = 'G';
        aa.age = 25;
        aa.sex = 10;
        
//        aa.hton();
//        
//        aa.ntoh((const char*) &aa);
//        
//        aa.isOk = false;
    }
    
    {///Pass
        struct TEST{
            char len;
            float str[100];
        };

        
        TEST tesaaa;
        tesaaa.len = 80;
        for (int i=0; i<tesaaa.len; i++) {
            tesaaa.str[i] = 10 * (i+1);
        }
        
        
        using L = decltype(TEST::len);
        using E = std::remove_pointer_t< std::decay_t< decltype( TEST::str)> >;
        using AY = endian:: Array<L, E>;
        AY list{(const char*)&tesaaa};
        
        assert(list.elements.size() == endian::translate<L>::nth(tesaaa.len));
        
        for (int i=0; i<list.elements.size(); i++) {
            assert(endian::translate<E>::nth(tesaaa.str[i])  == list.elements[i]);
        }
        
        auto net = list.hton();
        
        assert(net.size() == sizeof(L) + list.elements.size() * sizeof(E) );
        
        TEST *p = (TEST*)net.data();
        
        
        assert(p->len == tesaaa.len);
        for (int i=0; i<list.elements.size(); i++) {
            assert(p->str[i] == tesaaa.str[i]);
        }
    }
    
    
    {
        struct UYH{
            int aaa[2];
            bool cc;
            short dd;
            char xx;
            
            bool operator == (const UYH & other){
                for (int i=0; i<sizeof(UYH); i++) {
                    if ( *(((char*)this) + i) != *(((char*)&other) + i) ) {
                        return false;
                    }
                }
                return  true;
            }
            
            IMPLEMENT_ENDIAN(UYH,decltype(aaa),decltype(cc),decltype(dd),decltype(xx))
        };
        
        struct TEST{
            char len;
            UYH str[100];
        };

        
        TEST tesaaa;
        tesaaa.len = 80;
        for (int i=0; i<tesaaa.len; i++) {
            UYH tmp;
            tmp.aaa[0] = i*21;
            tmp.aaa[1] = i*43;
            tmp.cc = true;
            tmp.dd = 40;
            tmp.xx = 'D';
            tesaaa.str[i] = tmp;
        }
        
        
        using L = decltype(TEST::len);
        using E = std::remove_pointer_t< std::decay_t< decltype( TEST::str)> >;
        using AY = endian:: Array<L, E>;
        AY list{(const char*)&tesaaa};
        
        assert(list.elements.size() == endian::translate<L>::nth(tesaaa.len));
        
        for (int i=0; i<list.elements.size(); i++) {
            assert(endian::translate<int>::nth(tesaaa.str[i].aaa[0])  == list.elements[i].aaa[0]);
            assert(endian::translate<int>::nth(tesaaa.str[i].aaa[1])  == list.elements[i].aaa[1]);
        }
        
        auto net = list.hton();
        
        assert(net.size() == sizeof(L) + list.elements.size() * sizeof(E) );
        
        TEST *p = (TEST*)net.data();
        
        
        assert(p->len == tesaaa.len);
        for (int i=0; i<list.elements.size(); i++) {
            assert(p->str[i] == tesaaa.str[i]);
        }
    }
    
    
    {///Pass   normal 数组OK
        struct AHHH {
            bool isOk;
            char alpha[2];
            int64_t age[2];
            int xx;
            
            bool operator == (const AHHH & other){
                for (int i=0; i<sizeof(AHHH); i++) {
                    if ( *(((char*)this) + i) != *(((char*)&other) + i) ) {
                        return false;
                    }
                }
                return  true;
            }
            
            IMPLEMENT_ENDIAN(AHHH, decltype(isOk),decltype(alpha),decltype(age),decltype(xx))
        };
        
        AHHH adsaf{};
        adsaf.isOk = true;
        adsaf.alpha[0] = 'X';
        adsaf.alpha[1] = 'Y';
        adsaf.age[0] = 100;
        adsaf.age[1] = 200;
        adsaf.xx = 1000;
        
        
        auto data = adsaf.hton();
        AHHH *p = (AHHH*) data.data();
        
        AHHH hgas((const char*)data.data());
        assert(hgas == adsaf);
        
        assert(data.size() == sizeof(AHHH));
        assert(endian::translate<bool>::htn(adsaf.isOk) == p->isOk);
        assert(endian::translate<char>::htn(adsaf.alpha[0]) == p->alpha[0]);
        assert(endian::translate<char>::htn(adsaf.alpha[1]) == p->alpha[1]);
        assert(endian::translate<int64_t>::htn(adsaf.age[0]) == p->age[0]);
        assert(endian::translate<int64_t>::htn(adsaf.age[1]) == p->age[1]);
        assert(endian::translate<int>::htn(adsaf.xx) == p->xx);
        
    }
    
    {//Pass ，含结构体、type_list
        struct CCC {
            bool isOk[2];
            char cc[2];
            int64_t big;
            AAA aa;
            endian::type_list<int, bool> tyy;
            
            bool operator == (const CCC & other){
                for (int i=0; i<sizeof(CCC); i++) {
                    if ( *(((char*)this) + i) != *(((char*)&other) + i) ) {
                        return false;
                    }
                }
                return  true;
            }
            
            IMPLEMENT_ENDIAN(CCC,decltype(isOk),decltype(cc),decltype(big),decltype(aa),decltype(tyy))
        };
        
        CCC kcccc{};
        kcccc.isOk[0] = true;
        kcccc.isOk[1] = false;
        kcccc.cc[0] = 'D';
        kcccc.cc[1] = 'K';
        kcccc.big = 100;
        kcccc.aa = aa;
        kcccc.tyy.head = 120;
        kcccc.tyy.trail.head = true;
        
        auto data = kcccc.hton();
        assert(data.size() == sizeof(CCC));
        
        CCC *pp = (CCC*) data.data();
        CCC newCC((const char*) pp);
        assert(newCC == kcccc);
       
    }

    return 0;
}


#pragma pack(pop)

#include <iostream>
#include "endian.h"

#pragma pack(push, 1)

struct BBB {
    bool isOk;
    char alpha;
    int64_t age;
    short sex;
};


struct AAA {
    bool isOk;
    char alpha;
    int64_t age;
    short sex;

    IMPLEMENT_ENDIAN(AAA,decltype(isOk),decltype(alpha),decltype(age),decltype(sex))
};



struct AHHH {
    bool isOk;
    char alpha[2];
    int64_t age[2];
    int xx;
    
    IMPLEMENT_ENDIAN(AHHH, decltype(isOk),decltype(alpha),decltype(age),decltype(xx))
};

struct CCC {
    bool isOk[2];
    char cc[2];
    int64_t big;
    AAA aa;
    endian::type_list<int, bool> tyy;
    
    IMPLEMENT_ENDIAN(CCC,decltype(isOk),decltype(cc),decltype(big),decltype(aa),decltype(tyy))
};


#pragma pack(pop)


int main() {
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
    {///normal 数组OK
        AHHH adsaf{};
        adsaf.isOk = true;
        adsaf.alpha[0] = 'X';
        adsaf.alpha[1] = 'Y';
        adsaf.age[0] = 100;
        adsaf.age[1] = 200;
        adsaf.xx = 1000;
        
        adsaf.hton();
        
        adsaf.ntoh((const char*)&adsaf);
        
        adsaf.isOk = true;
    }
    
    {//OK，含结构体、type_list
        CCC kcccc{};
        kcccc.isOk[0] = true;
        kcccc.isOk[1] = false;
        kcccc.cc[0] = 'D';
        kcccc.cc[1] = 'K';
        kcccc.big = 100;
        kcccc.aa = aa;
        kcccc.tyy.head = 120;
        kcccc.tyy.trail.head = true;
        
        kcccc.hton();
      
        kcccc.ntoh((const char*) &kcccc);
       
        kcccc.isOk[0] = true;
    }

    return 0;
}

//
//  main.cpp
//  C++Timer
//
//  Created by goggle on 2024/10/19.
//

#include <iostream>
#include "timer.hpp"

template <typename ...T>
void printTimer(T&& ... arg){
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm* local_time = std::localtime(&now_time_t);

    std::cout << "当前时间: "<< std::put_time(local_time, "%H:%M:%S")<< '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
    
    (std::cout << ... << arg) << std::endl;
}

int main(int argc, const char * argv[]) {
    timer::Timer  t{};
    
    
    {
        int i = 0;
        timer::timer_id id;
        id =  t.scheduledTimer(std::chrono::seconds(10), [&](){
            printTimer(" Hello, World! ",i++);
            
            t.scheduledTimer(std::chrono::seconds(5), [&](){
                printTimer(" enbenddd------enbenad ",i++);
                
                t.removeTimer(id);
                
                return std::chrono::seconds(0);
            });
            
            return std::chrono::seconds(5);
        });
    }
    
    {
        int i = 0;
        
        t.scheduledTimer(std::chrono::seconds(5), [&](){
           
            printTimer(" ---every 5 sec--! ",i++ );
            return std::chrono::seconds(5);
        });
    }
    
    
    {
        int i = 0;
        
        t.scheduledTimer(std::chrono::seconds(1), [&](){
            printTimer("=================== ",i++ );
            return std::chrono::seconds(1);
        });
    }
    printTimer("start--");
   
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    return 0;
}

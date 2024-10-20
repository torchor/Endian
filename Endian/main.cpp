//
//  main.cpp
//  C++Timer
//
//  Created by goggle on 2024/10/19.
//

#include <iostream>
#include <map>
#include <functional>
#include <thread>
#include <chrono>
#include <iomanip>
#include <ctime>

class timer{
public:
    using timer_id = u_int64_t;///高48位表示距离base_time时间戳，低16位区分同一个时间，的不同Timer
    using callback = std::function<bool(void)>;///返回true，定时器继续，返回false，取消定时器
    
  
    timer():base_time(std::chrono::steady_clock::now()),keySufix(0){
        thread = std::thread([this](){
            std::unique_lock<std::mutex> lock(mutex);
            do {
                condition.wait(lock,[this](){
                    return !map.empty();
                });
  
                auto first = map.begin();
                auto timerId = first->first;
                auto nd = first->second;
                
                auto expire_time = base_time + std::chrono::milliseconds(timerId >> (sizeof(keySufix)*8));

                condition.wait_until(lock, expire_time);
                
                if (map.empty()) {
                    continue;
                }
                
                if (timerId != map.begin()->first) {///醒来的时候，之前的定时器已经被删除了，开始处理下一个
                    continue;
                }
                
                if (std::chrono::steady_clock::now() < expire_time) {///not expired yet, ensure it's timeout not a spurious wakeup
                    continue;
                }
                
                auto should_repeat =  first->second.call();
                map.erase(timerId);
                if (should_repeat) {///setup next fire time
                    create_timer(nd);
                }else{///clear timer
                    index_map.erase(nd.orignal_id);
                }
                
            } while (true);
        });
    }
    
   
    timer_id scheduledTimer(std::chrono::milliseconds timeout,callback call){
        node nd;
        nd.call = call;
        nd.timeout = timeout;
        nd.orignal_id = 0;
       
        create_timer(nd);
        condition.notify_one();
        return nd.orignal_id;
    }
    
    void  removeTimer(timer_id id){
        auto isOnThread = std::this_thread::get_id() == thread.get_id();
        
        auto fn = [&](){
            auto key = index_map.find(id);
            if (key != index_map.end()) {
                map.erase(key->second);
                index_map.erase(id);
            }
        };
        
        if(!isOnThread){
            std::lock_guard<std::mutex> guard(mutex);
            fn();
        }else{
            fn();
        }
    }
    
    
    ~timer(){
        if (thread.joinable()) {
            thread.join();
        }
    }
private:
    struct node{
        timer_id orignal_id;
        callback call;
        std::chrono::milliseconds timeout;///每隔多久触发一次
    };
    
    void create_timer(node& parameter){
        auto isOnThread = std::this_thread::get_id() == thread.get_id();
        bool retry = true;
        timer_id newId = 0;
        while (retry) {
            retry = false;
            
            auto expired = std::chrono::steady_clock::now() - base_time + parameter.timeout;
            u_int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(expired).count();
            if (!isOnThread){
                std::lock_guard<std::mutex> guard(mutex);
                keySufix ++;
            }else{
                keySufix ++;
            }
            newId = (milliseconds << (sizeof(keySufix)*8)) | keySufix;
            if (0 == parameter.orignal_id) {///first time save as it's orginal id
                parameter.orignal_id = newId;
            }
            
            if (!isOnThread) {
                std::lock_guard<std::mutex> guard(mutex);
                if (map.find(newId) != map.end()) {///已经存在这个key, 需要重写生成一个唯一key
                    retry = true;
                    continue;
                }
                index_map[parameter.orignal_id] = newId;
                map[newId] = parameter;
            }else{
                if (map.find(newId) != map.end()) {///已经存在这个key, 需要重写生成一个唯一key
                    retry = true;
                    continue;
                }
                index_map[parameter.orignal_id] = newId;
                map[newId] = parameter;
            }
            
        }
    }
    

    std::mutex mutex;
    std::condition_variable condition;
    
    std::thread thread;
    std::map<timer_id,node> map;///自动按时间排序

    std::unordered_map<timer_id, timer_id> index_map;///orinalId ---> timerId
    
    const std::chrono::time_point<std::chrono::steady_clock> base_time;
    u_int16_t keySufix;///一直递增，避免同一时间生成的key 一样
};

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
    timer t{};
    
    {
        int i = 0;
        timer::timer_id id;
        id =  t.scheduledTimer(std::chrono::seconds(10), [&](){
            printTimer(" Hello, World! ",i++);
            
            t.scheduledTimer(std::chrono::seconds(5), [&](){
                printTimer(" enbenddd------enbenad ",i++);
                
                t.removeTimer(id);
                
                return false;
            });
            
            return true;
        });
    }
    
    {
        int i = 0;
        
        t.scheduledTimer(std::chrono::seconds(5), [&](){
           
            printTimer(" ---every 5 sec--! ",i++ );
            return true;
        });
    }
    
    
    {
        int i = 0;
        
        t.scheduledTimer(std::chrono::seconds(1), [&](){
            printTimer("=================== ",i++ );
            return true;
        });
    }
    printTimer("start--");
   
    return 0;
}

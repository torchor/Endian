//
//  lock_free.h
//  lock-free algorithms and data structures
//
//  Created by Matthew on 2026/6/20.
//

#ifndef ___lock___free___h____
#define ___lock___free___h____

#include <atomic>
#include <stdexcept>
#include <memory>
#include <assert.h>

namespace lock_free {

constexpr auto max_hazard_pointers=128;
struct hazard_pointer
{
    std::atomic_flag occupied;
    std::atomic<void*> pointer;
};

class hp_owner
{
    hazard_pointer* hp;
public:
   inline static hazard_pointer hazard_slots[max_hazard_pointers];
    
    hp_owner(hp_owner const&)=delete;
    hp_owner operator=(hp_owner const&)=delete;
    hp_owner():hp(nullptr)
    {
        for(unsigned i=0;i<max_hazard_pointers;++i)
        {
            if(!hazard_slots[i].occupied.test_and_set())
            {
                hp=&hazard_slots[i];
                return;
            }
        }
        throw std::runtime_error("No hazard pointers available");
    }
    inline std::atomic<void*>& get_pointer()
    {
        return hp->pointer;
    }
    ~hp_owner()
    {
        hp->pointer.store(nullptr);
        hp->occupied.clear();
    }
};

template<uint8_t index=0>
inline std::atomic<void*>& get_hp_cache_for_current_thread()
{
    thread_local static hp_owner hazard;
    return hazard.get_pointer();
}


inline std::atomic<void*>& get_hazard_pointer_for_current_thread(std::unique_ptr<hp_owner>&ptr)
{
    auto&&cache = get_hp_cache_for_current_thread();
    if (cache.load() == nullptr) {   // ← 槽位空闲，直接用
        return cache;
    }
    auto pNew = std::make_unique<hp_owner>();///从全局槽中，临时申请一个新的
    ptr.swap(pNew);
    return  ptr->get_pointer();
}

template<typename T,typename = std::enable_if_t<!std::is_void_v<T> && !std::is_pointer<T>::value >>
struct data_to_reclaim
{
    T* data;
    data_to_reclaim* next;
    data_to_reclaim(T* p):data(p),next(0){}
    virtual ~data_to_reclaim(){delete data;}
};
struct empty_type{};
using retire_node = data_to_reclaim<empty_type>;
inline std::atomic<retire_node*> nodes_to_reclaim;
inline void add_to_reclaim_list(retire_node* node)
{
    node->next=nodes_to_reclaim.load();
    while(!nodes_to_reclaim.compare_exchange_weak(node->next,node));
}

inline bool outstanding_hazard_pointers_for(void* p)
{
  for(unsigned i=0;i<max_hazard_pointers;++i)
  {
    if(hp_owner::hazard_slots[i].pointer.load()==p)
    {
      return true;
    }
}
  return false;
}

template<typename T,typename = std::enable_if_t<!std::is_void_v<T> && !std::is_pointer<T>::value >>
inline void reclaim_later(T* data)
{
    add_to_reclaim_list(reinterpret_cast<retire_node*>(new data_to_reclaim<T>(data)));
}
inline void delete_nodes_with_no_hazards()
{
    auto current=nodes_to_reclaim.exchange(nullptr);
    while(current)
    {
        auto const next=current->next;
        if(!outstanding_hazard_pointers_for(current->data))
        {
            delete current;
        }
        else
        {
            add_to_reclaim_list(current);
        }
        current=next;
    }
}

template<typename T>
class stack
{
private:
    struct node {
        std::shared_ptr<T> data; // 1 指针获取数据
        node* next;
        node(T const& data_):
        data(std::make_shared<T>(data_)) // 2 让std::shared_ptr指向新分配出来的T
        {}
    };
    std::atomic<node*> head{};
public:
    inline void push(T const& data)
    {
        node* const new_node=new node(data);
        new_node->next=head.load();
        while(!head.compare_exchange_weak(new_node->next,new_node));
    }
    inline std::shared_ptr<T> pop()
    {
        std::unique_ptr<hp_owner> p_hp;///如果是动态申请的槽，析构时自动还到全局池中
        std::atomic<void*>&hp=get_hazard_pointer_for_current_thread(p_hp);
        node* old_head=head.load();
        do
        {
            node* temp;
            do // 1 直到将风险指针设为head指针
            {
                temp=old_head;
                hp.store(old_head);
                old_head=head.load();
            } while(old_head!=temp);
        }
        while(old_head && !head.compare_exchange_strong(old_head,old_head->next));
        hp.store(nullptr); // 2 当声明完成，清除风险指针
        std::shared_ptr<T> res;
        if(old_head)
        {
            res.swap(old_head->data);
            if(outstanding_hazard_pointers_for(old_head)) // 3 在删除之前 对风险指针引用的节点进行检查
            {
                reclaim_later(old_head);  // 4
            }
            else
            {
                delete old_head;  // 5
            }
            delete_nodes_with_no_hazards();  // 6
        }
        return res;
    }
    
    ~stack()
    {
        while (pop()) {}
        // 最后一批退役节点没有后续 pop 来触发回收，这里补一次终扫，避免节点壳泄漏
        delete_nodes_with_no_hazards();
    }
};

/// A thread-safe atomic pointer that owns its pointee.
/// Automatically defers deletion of the old value using hazard pointers,
/// ensuring no thread is accessing it before it is freed.
///
/// Usage:
///   atomic_owner_ptr<Foo> ptr(new Foo());
///   auto lock = ptr.safe_read();   // safe concurrent read
///   lock->bar();                   // access via RAII lock
///   ptr = new Foo();               // old value safely reclaimed
template<typename  T>
struct atomic_owner_ptr{
    struct hazard_lock
    {
        /// 风险指针获取协议：写入风险槽后回读源原子量校验，直到“槽内==源”为止；
        /// 否则在写槽之前对象可能已被并发写者释放，造成 use-after-free。
        explicit hazard_lock(std::atomic<const T*>& src)
            :rawPointer(nullptr),hp(get_hazard_pointer_for_current_thread(p_hp))
        {
            const T* ptr = src.load();
            do {
                rawPointer = ptr;
                hp.store(const_cast<T*>(ptr));
                ptr = src.load();
            } while (ptr != rawPointer);
        }
        hazard_lock(hazard_lock &&v) noexcept :rawPointer(v.rawPointer),hp(v.hp),p_hp(std::move(v.p_hp)){v.rawPointer = nullptr;}
        hazard_lock(const hazard_lock&)=delete;
        hazard_lock& operator=(const hazard_lock&)=delete;

        const T* operator ->() const {return rawPointer;}
        const T& operator*()   const { return *rawPointer; }
        const T* get()         const { return rawPointer; }
        explicit operator bool() const { return rawPointer != nullptr; }

        ~hazard_lock(){
            if (rawPointer) {
                hp.store(nullptr); ///  当声明完成，清除风险指针
            }
        }
    private:
        std::unique_ptr<hp_owner> p_hp{};//全局槽中动态临时申请一个,当前hazard_lock对象释放后，将自动还回全局槽中
        const T *rawPointer;
        std::atomic<void*> &hp;///默认从当前线程的Local_thread中拿，如果已经被使用了则去全局槽中动态临时申请一个
    };
    
    ///safe read
    hazard_lock safe_read(){return hazard_lock(p);}
    
    atomic_owner_ptr(const T*_p):p(_p){}
    ~atomic_owner_ptr(){*this = nullptr;}
    
    atomic_owner_ptr& operator=(const T*_p)
    {
        auto old = p.load();
        while (!p.compare_exchange_weak(old, _p));
        if (auto raw = const_cast<T*>(old)) {
            if(outstanding_hazard_pointers_for(raw))
            {
                reclaim_later(raw);
            }
            else
            {
                delete raw;
            }
            delete_nodes_with_no_hazards();
        }
        return *this;
    }
    
private:
    std::atomic<const T*> p{};
};

}

#endif /* lock_free_h */

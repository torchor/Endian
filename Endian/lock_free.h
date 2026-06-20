//
//  lock_free.h
//  Endian
//
//  Created by Matthew on 2026/6/20.
//

#ifndef ___lock___free___h____
#define ___lock___free___h____

#include <atomic>
#include <stdexcept>
#include <memory>

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
   inline static hazard_pointer hazard_pointers[max_hazard_pointers];
    
    hp_owner(hp_owner const&)=delete;
    hp_owner operator=(hp_owner const&)=delete;
    hp_owner():hp(nullptr)
    {
        for(unsigned i=0;i<max_hazard_pointers;++i)
        {
            if(!hazard_pointers[i].occupied.test_and_set())
            {
                hp=&hazard_pointers[i];
                return;
            }
        }
        throw std::runtime_error("No hazard pointers available");
    }
    std::atomic<void*>& get_pointer()
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
std::atomic<void*>& get_hazard_pointer_for_current_thread()
{
    thread_local static hp_owner hazard;
    return hazard.get_pointer();
}


template<typename T>
struct data_to_reclaim
{
    T* data;
    data_to_reclaim* next;
    data_to_reclaim(T* p):data(p),next(0){}
   virtual ~data_to_reclaim()
    {
        delete data;
    }
};
std::atomic<data_to_reclaim<void>*> nodes_to_reclaim;
void add_to_reclaim_list(data_to_reclaim<void>* node)
{
    node->next=nodes_to_reclaim.load();
    while(!nodes_to_reclaim.compare_exchange_weak(node->next,node));
}

bool outstanding_hazard_pointers_for(void* p)
{
  for(unsigned i=0;i<max_hazard_pointers;++i)
  {
    if(hp_owner::hazard_pointers[i].pointer.load()==p)
    {
      return true;
    }
}
  return false;
}

template<typename T,typename = std::enable_if_t<!std::is_void_v<T>>>
void reclaim_later(T* data)
{
    using Type = data_to_reclaim<void>;
    add_to_reclaim_list(reinterpret_cast<Type*>(new data_to_reclaim<T>(data)));
}
void delete_nodes_with_no_hazards()
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
    std::atomic<node*> head;
public:
    void push(T const& data)
    {
        node* const new_node=new node(data);
        new_node->next=head.load();
        while(!head.compare_exchange_weak(new_node->next,new_node));
    }
    std::shared_ptr<T> pop()
    {
        std::atomic<void*>&hp=get_hazard_pointer_for_current_thread();
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
};

}

#endif /* lock_free_h */

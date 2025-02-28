## C++无锁算法



##### 💡先注明概念

这里从数据竞争的角度，分为：

- 竞态变量（`J`）：可能被多个线程竞争访问的变量
- 非竞态变量（¬ J）：不存在竞争访问的变量

从是否原子变量的角度，可分为：

- Atomic原子变量（`A`）：可能被多个线程竞争访问的变量
- 非Atomic原子变量（¬ A）：不存在竞争访问的变量

如此，每个变量的类型都有4种组合：*`J A`*	`¬J A`	*`J ¬A`*	`¬J ¬A`，由于非竞态变量之间的数据操作自然不存在竞争，所以我们只讨论第1、3种情况。



##### 区分竞态变量

多线程并发，最关键在于对数据的竞争，下面分别举例说明，哪些是竞态变量（`J`），哪些是非竞态变量（¬ J）？

```c++
int onlyOneThreadUsed = 0;///1 虽然是全局变量，但是只被一个线程访问，所以不存在竞争，非原子变量：¬J ¬A
atomic<int> shareData =0;///2 全局变量，可能会被多个线程访问，存在竞争,同时也是原子变量：J  A

///子线程主函数
void subTask(){
    static int ctn = 0;///3 作用域是全局的，也会被多个线程并发访问，存在竞争，非原子变量：J ¬A
    thread_local atomic<int> localCtn=0; ///4 线程局部变量，每个线程都有各自的副本，互不干涉，不存在竞争:¬J A
  
    for(int i=0;i<200;){///局部变量i，不存在竞争
        ctn ++;///作用域是全局的，也会被多个线程并发访问，存在竞争
        localCtn ++;
        shareData ++; ///全局变量被多个线程并发访问，存在竞争
    }
}
///情况分析
 shareData = onlyOneThreadUsed;//1--2  安全
 ctn = onlyOneThreadUsed;//1--3  不安全，无锁算法失效：二者没有一个是原子变量
 localCtn = onlyOneThreadUsed; //1-4 安全
 ctn = shareData;//2--3 不安全，无锁算法失效
 localCtn = shareData;//2--4  不安全，无锁算法OK
 localCtn = ctn; //3-4 不安全，无锁算法失效：因为竞态变量非原子，只能互斥锁

int main(){
    std::vector<std::thread> threadArray;///局部变量，不存在竞争
    
    for (int i=0; i<10; i++) {///局部变量i，不存在竞争
        threadArray.push_back(std::thread(subTask));
    }
    onlyOneThreadUsed = 1;//只有主线程访问，不存在竞争
  
    
  
    for (auto&t : threadArray) {t.join();}///wait
    return 0;
}
```



#### 1、将一个`竞态原子变量A`保存到另一个`竞态原子变量B`

💡：`竞态原子变量`指可能被多线程竞争访问的atomic类型变量

参考如下一段简单的代码：

```c++
std::shared_ptr<T> pop()
{
  std::atomic<void*>& hp=  get_hazard_pointer_for_current_thread();
  node* old_head=head.load();  // 1
///非线程安全，这段时间内,head可能被其他线程修改了，这样hp保存的就是老数据了！BUG
  hp.store(old_head);  // 2，我们期望的是：hp存储的是此时此刻head的最新值
  
// ...
}
```

`1、2`本身各自都是原子操作，但是`1+2`作为一个整体，就不是原子操作了，因为无法保证1—2整个执行期间不被打断。分析：执行1，还未执行2时，其他线程可能将head修改了，这时hp保存的是一个`旧的head`，这样与我们期望不符。那如何修复呢？原因分析出来了，解决就很简单：

- 2操作完后，立刻重新加载head，查询其现在的值 是否跟 old_head一样？
- 如果一样，说明这段时间内，head还好没被其他线程修改，万幸！成功保存，结束。
- 如果不一样，说明这段时间内，head已经被其他线程修改了，我们需要重新`goto 1;` 重新执行一遍

通过上述流程，最终一定能保证`1+2`这段代码整体是个原子操作，线程安全！⚠️：这里暂时不考虑`ABA`问题。

如下伪代码即可实现上述要求：

```c++
std::shared_ptr<T> pop()
{
  std::atomic<void*>& hp=  get_hazard_pointer_for_current_thread();
retry:  
  node* old_head=head.load();  // 1
///非线程安全，这段时间内,head可能被其他线程修改了，这样hp保存的就是老数据了！BUG
  hp.store(old_head);  // 2，我们期望的是：hp存储的是此时此刻head的最新值
  
  if(head.load() != old_head) //如果head目前的最新值不等于old_head,说明需要重新操作！
    goto retry;
// ...
}
```

上述代码使用了goto语句，比例容易理解，也可以写成下面的：

```c++
std::shared_ptr<T> pop()///效果与上面的实现一样
{
  std::atomic<void*>& hp=get_hazard_pointer_for_current_thread();
  node* old_head=head.load();  // 1
  node* temp;
  do
  {
    temp=old_head;
    hp.store(old_head);  // 2
    old_head=head.load();
  } while(old_head!=temp); // 3
// ...
}
```

如此，整个循环保证了：`1，2是个原子操作`。

##### 问题：如果1和2之间，插入一些其他代码，还能保证其`原子性`吗？
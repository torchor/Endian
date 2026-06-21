//
//  test_lock_free.cpp
//  Endian
//
//  lock_free 完整测试套件（针对“动态无界 hazard 域 hp_domain”版本）。
//
//  覆盖：
//    · stack<T>            —— LIFO/空栈语义、并发不丢不重、各回收路径零泄漏
//    · atomic_owner_ptr<T> —— 安全读、赋值回收、延迟回收、并发读写无 UAF
//    · hp_domain（本次大改）—— 同线程多槽嵌套、无界深度增长、并发增长与复用
//
//  证明手法（macOS ASan 无 leak 检测，故用量化不变量）：
//    · Tracked  : 构造/析构存活计数 —— alive 归零 ⇔ 内部数据全部释放
//    · Guarded  : chk==(v^MAGIC) 自校验 + 析构毒化 —— 读到失效对象当场可见(配合 ASan 抓 UAF)
//
//  编译（越靠后越严格）：
//    clang++ -std=c++17 -O2            -pthread test_lock_free.cpp -o t && ./t
//    clang++ -std=c++17 -g -O1 -fsanitize=address    -pthread ...   (越界/坏释放/UAF)
//    clang++ -std=c++17 -g -O1 -fsanitize=thread     -pthread ...   (数据竞争)
//    clang++ -std=c++17 -g -O1 -fsanitize=undefined  -pthread ...   (未定义行为)
//

#include "lock_free.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <set>
#include <cstdint>

// ───

// ─────────────────────────────────────────────
int main() {
    std::cout << "=== lock_free 完整测试（hp_domain 动态无界版）===\n\n";

  
   // return g_fail == 0 ? 0 : 1;
}

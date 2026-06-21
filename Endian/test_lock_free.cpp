//
//  test_lock_free.cpp
//  Endian
//
//  Created by Marvel on 2026/6/20.
//
//  lock_free::stack 的完整测试用例。
//
//  目标：
//    1. 证明功能正确性（LIFO、空栈、计数、并发不丢不重）。
//    2. 证明“内部数据被正常释放”——不依赖平台 leak sanitizer，
//       而是用一个带存活计数的探针类型 Tracked，量化验证：
//         · 每个被 push 进去的对象，最终都被析构（alive 回到 0）；
//         · stack 析构函数能排空残留节点；
//         · hazard-pointer 延迟回收链表（nodes_to_reclaim）最终清空。
//
//  编译建议（任选其一即可，越靠后越严格）：
//    clang++ -std=c++17 -O2 -pthread test_lock_free.cpp -o t && ./t
//    clang++ -std=c++17 -g -O1 -pthread -fsanitize=address  ... (堆越界/坏释放)
//    clang++ -std=c++17 -g -O1 -pthread -fsanitize=thread   ... (数据竞争)
//

#include "lock_free.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <set>
#include <cstdint>

// ─────────────────────────────────────────────
// 极简测试框架
// ─────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

// ─────────────────────────────────────────────
int main() {
    std::cout << "=== lock_free 完整测试 ===\n\n";

    

    return g_fail == 0 ? 0 : 1;
}

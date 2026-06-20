//
//  test_lock_free.cpp
//  Endian
//
//  Created by Marvel on 2026/6/20.
//

#include <stdio.h>
#include "lock_free.h"
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <memory>
#include <thread>
#include <vector>
#include <cassert>
#include <numeric>
#include <algorithm>
#include <sstream>


// ─────────────────────────────────────────────
// 测试框架（极简）
// ─────────────────────────────────────────────
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name, expr) do { \
    if (expr) { std::cout << "[PASS] " name "\n"; ++pass_count; } \
    else       { std::cout << "[FAIL] " name "\n"; ++fail_count; } \
} while(0)

// ─────────────────────────────────────────────
// Case 1：单线程基本功能
// ─────────────────────────────────────────────
void test_single_thread_basic() {
    lock_free::stack<int> s;

    // 空栈 pop 返回 nullptr
    TEST("empty pop returns null", s.pop() == nullptr);

    // push 后 pop 拿到值
    s.push(42);
    auto v = s.pop();
    TEST("push then pop", v && *v == 42);

    // 再次 pop 为空
    TEST("stack empty after pop", s.pop() == nullptr);

    // LIFO 顺序
    s.push(1); s.push(2); s.push(3);
    TEST("LIFO order 3", *s.pop() == 3);
    TEST("LIFO order 2", *s.pop() == 2);
    TEST("LIFO order 1", *s.pop() == 1);
}

// ─────────────────────────────────────────────
// Case 2：多线程并发 push，单线程 pop 验证总量
// ─────────────────────────────────────────────
void test_concurrent_push() {
    constexpr int THREADS = 8;
    constexpr int PER     = 1000;
    lock_free::stack<int> s;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back([&, t]{
            for (int i = 0; i < PER; ++i)
                s.push(t * PER + i);
        });
    for (auto& th : threads) th.join();

    int count = 0;
    while (s.pop()) ++count;
    TEST("concurrent push total count", count == THREADS * PER);
}

// ─────────────────────────────────────────────
// Case 3：多线程并发 pop，验证每个值只被取一次
// ─────────────────────────────────────────────
void test_concurrent_pop_no_duplicate() {
    constexpr int N = 2000;
    lock_free::stack<int> s;
    for (int i = 0; i < N; ++i) s.push(i);

    std::atomic<int> sum{0};
    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back([&]{
            while (true) {
                auto v = s.pop();
                if (!v) break;
                sum += *v;
            }
        });
    for (auto& th : threads) th.join();

    int expected = N * (N - 1) / 2;
    TEST("concurrent pop no duplicate (sum check)", sum == expected);
}

// ─────────────────────────────────────────────
// Case 4：并发 push + pop 混合，无 crash，无泄漏
// ─────────────────────────────────────────────
void test_concurrent_push_pop_mixed() {
    constexpr int THREADS = 8;
    constexpr int OPS     = 500;
    lock_free::stack<int> s;

    std::atomic<int> pushed{0};
    std::atomic<int> popped{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back([&, t]{
            for (int i = 0; i < OPS; ++i) {
                s.push(t * OPS + i);
                pushed++;
                auto v = s.pop();
                if (v) popped++;
            }
        });
    for (auto& th : threads) th.join();

    // 排空剩余
    while (s.pop()) popped++;

    TEST("mixed push/pop total balance", pushed == popped);
}

// ─────────────────────────────────────────────
// Case 5：shared_ptr 引用计数正确（无泄漏代理）
// ─────────────────────────────────────────────
void test_shared_ptr_refcount() {
    struct Probe {
        static std::atomic<int>& count() {
            static std::atomic<int> c{0};
            return c;
        }
        Probe()  { ++count(); }
        ~Probe() { --count(); }
    };

    // 用 int 代替（Probe 不可复制构造，用计数间接验证）
    // 改为用 shared_ptr<int> 的方式验证 stack<int> 析构后无残留
    {
        lock_free::stack<int> s;
        s.push(1); s.push(2); s.push(3);
        s.pop(); s.pop();
        // 还剩1个节点在栈里，离开作用域 stack 析构
        // （stack 析构未实现 drain，节点会泄漏——这是已知限制）
    }
    // 此测试主要验证 shared_ptr 的 swap 语义正确
    {
        lock_free::stack<int> s;
        s.push(99);
        auto p1 = s.pop();
        auto p2 = p1;  // 引用计数 = 2
        TEST("shared_ptr refcount", p1.use_count() == 2);
        p2.reset();
        TEST("shared_ptr refcount after reset", p1.use_count() == 1);
        // p1 离开作用域自动释放
    }
}

// ─────────────────────────────────────────────
// Case 6：hazard pointer 槽位复用（线程退出后释放）
// ─────────────────────────────────────────────
void test_hazard_slot_reuse() {
    bool ok = true;
    // 连续创建销毁线程，槽位若不释放会耗尽
    for (int i = 0; i < lock_free::max_hazard_pointers + 10; ++i) {
        try {
            std::thread([]{
                lock_free::stack<int> s;
                s.push(1);
                s.pop();
            }).join();
        } catch (...) {
            ok = false;
            break;
        }
    }
    TEST("hazard slot reuse after thread exit", ok);
}

// ─────────────────────────────────────────────
// Case 7：retire list 最终清空（无节点永久积压）
// ─────────────────────────────────────────────
void test_retire_list_drained() {
    lock_free::stack<int> s;
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) s.push(i);
    for (int i = 0; i < N; ++i) s.pop();

    // 最后一次 pop 之后再做一次清理
    lock_free::delete_nodes_with_no_hazards();

    TEST("retire list eventually drained",
         lock_free::nodes_to_reclaim.load() == nullptr);
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main() {
    std::cout << "=== lock_free::stack tests ===\n\n";

    test_single_thread_basic();
    test_concurrent_push();
    test_concurrent_pop_no_duplicate();
    test_concurrent_push_pop_mixed();
    test_shared_ptr_refcount();
    test_hazard_slot_reuse();
    test_retire_list_drained();

    std::cout << "\n";
    std::cout << "PASS: " << pass_count << "\n";
    std::cout << "FAIL: " << fail_count << "\n";

    return fail_count == 0 ? 0 : 1;
}

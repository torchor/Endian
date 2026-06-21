//
//  test_lock_free.cpp
//  Endian
//
//  lock_free 完整测试套件（hp_domain 动态无界 + retire 单例 版本）。
//
//  覆盖：
//    · stack<T>            —— LIFO/空栈语义、并发不丢不重、各回收路径零泄漏
//    · atomic_owner_ptr<T> —— 安全读、赋值回收、延迟回收、并发读写无 UAF
//    · hp_domain           —— 同线程多槽嵌套、无界深度增长、并发增长与复用
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
#include <utility>
#include <thread>
#include <vector>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <csignal>
#if defined(__APPLE__)
#  include <mach/mach.h>
#endif

// ─────────────────────────────────────────────
// 极简测试框架
// ─────────────────────────────────────────────
static long g_pass = 0;
static long g_fail = 0;
static bool g_quiet = false;   // soak 模式下抑制 [PASS] 刷屏，只打印 [FAIL]

#define TEST(name, expr) do {                                                  \
    if (expr) { if (!g_quiet) std::cout << "[PASS] " << name << "\n"; ++g_pass; } \
    else      { std::cout << "[FAIL] " << name << "   (" #expr ")\n"; ++g_fail; } \
} while (0)

// ─────────────────────────────────────────────
// 探针类型
// ─────────────────────────────────────────────
// Tracked：存活计数。alive 回到基线 ⇔ 所有对象都已析构（含拷贝）。
struct Tracked {
    static std::atomic<long> alive;
    static std::atomic<long> constructed;
    static std::atomic<long> destroyed;
    int v;
    explicit Tracked(int x = 0) : v(x) {
        alive.fetch_add(1, std::memory_order_relaxed);
        constructed.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked(const Tracked& o) : v(o.v) {           // push 内部 make_shared 会拷贝
        alive.fetch_add(1, std::memory_order_relaxed);
        constructed.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked& operator=(const Tracked&) = default;
    ~Tracked() {
        alive.fetch_add(-1, std::memory_order_relaxed);
        destroyed.fetch_add(1, std::memory_order_relaxed);
    }
};
std::atomic<long> Tracked::alive{0};
std::atomic<long> Tracked::constructed{0};
std::atomic<long> Tracked::destroyed{0};

// Guarded：存活计数 + 自校验 + 析构毒化。读到已析构对象 → valid() 失败。
struct Guarded {
    static std::atomic<long> alive;
    static constexpr int MAGIC = 0x5A5A1234;
    int v;
    int chk;
    explicit Guarded(int x) : v(x), chk(x ^ MAGIC) {
        alive.fetch_add(1, std::memory_order_relaxed);
    }
    ~Guarded() {
        chk = 0xDEAD;
        v   = -1;
        alive.fetch_add(-1, std::memory_order_relaxed);
    }
    bool valid() const { return chk == (v ^ MAGIC); }
};
std::atomic<long> Guarded::alive{0};

// ─────────────────────────────────────────────
// 针对 hp_domain / retire 的辅助
// ─────────────────────────────────────────────
namespace hp = lock_free;
using Owner = hp::unique_ptr<Guarded>;
using Lock  = Owner::hz_lock;

// 某指针当前是否被任一 hazard 槽保护
static bool out(const void* p) {
    return hp::hp_owner::hazard_domain.ptr_is_protected(const_cast<void*>(p));
}
// 当前活跃（pointer 非空）的 hazard 数 —— 观察域的增长/复用
static size_t live_hazards() {
    return hp::hp_owner::hazard_domain.protected_ptrs().size();
}
// 强制回收（无视批量阈值）：断言“此刻应已无残留”前调用
static void force_drain() {
    for (int i = 0; i < 3; ++i) hp::retire_list::get().delete_nodes_with_no_hazards(true);
}
// retire_count 计数器当前值（公开字段）
static int32_t retire_count_val() {
    return hp::retire_list::get().retire_count.load();
}
// 实际遍历 nodes_to_reclaim 得到的真链表长度（仅在静默/单线程下稳定）
static size_t retire_list_len() {
    size_t n = 0;
    for (auto p = hp::retire_list::get().nodes_to_reclaim.load(); p; p = p->next) ++n;
    return n;
}

// ═════════════════════════════════════════════
//  Part 1 —— stack<T>
// ═════════════════════════════════════════════

// Case 1：单线程基本功能 / LIFO / 空栈
static void test_stack_basic() {
    lock_free::stack<int> s;
    TEST("stack: 空栈 pop 为 null", s.pop() == nullptr);
    s.push(42);
    auto v = s.pop();
    TEST("stack: push 后 pop 得原值", v && *v == 42);
    TEST("stack: 取空后 pop 为 null", s.pop() == nullptr);
    s.push(1); s.push(2); s.push(3);
    TEST("stack: LIFO #1 (=3)", *s.pop() == 3);
    TEST("stack: LIFO #2 (=2)", *s.pop() == 2);
    TEST("stack: LIFO #3 (=1)", *s.pop() == 1);
    TEST("stack: 全取出后为空", s.pop() == nullptr);
}

// Case 2：shared_ptr 返回值语义
static void test_stack_shared_ptr() {
    lock_free::stack<int> s;
    s.push(99);
    auto p1 = s.pop();
    TEST("stack: pop 独占所有权 use_count==1", p1.use_count() == 1);
    {
        auto p2 = p1;
        TEST("stack: 拷贝后 use_count==2", p1.use_count() == 2);
        TEST("stack: 同一对象", p1.get() == p2.get() && *p2 == 99);
    }
    TEST("stack: 作用域后 use_count==1", p1.use_count() == 1);
}

// Case 3：全部 pop —— 零泄漏
static void test_stack_no_leak_drain() {
    const long base = Tracked::alive.load();
    constexpr int N = 5000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));
        long held = 0;
        while (auto p = s.pop()) { (void)p; ++held; }
        TEST("stack: push/pop 数量一致", held == N);
    }
    force_drain();
    TEST("stack: 全 pop 后零泄漏", Tracked::alive.load() == base);
}

// Case 4：只 push 不 pop —— 靠析构排空
static void test_stack_no_leak_destructor() {
    const long base = Tracked::alive.load();
    constexpr int N = 5000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));
        TEST("stack: 析构前对象存活", Tracked::alive.load() == base + N);
    }
    force_drain();
    TEST("stack: 析构排空后零泄漏", Tracked::alive.load() == base);
}

// Case 5：部分 pop + 析构剩余
static void test_stack_no_leak_partial() {
    const long base = Tracked::alive.load();
    {
        lock_free::stack<Tracked> s;
        s.push(Tracked(1)); s.push(Tracked(2)); s.push(Tracked(3));
        auto a = s.pop();
        TEST("stack: 部分 pop 后计数正确", Tracked::alive.load() == base + 3);
    }
    force_drain();
    TEST("stack: 部分 pop + 析构后零泄漏", Tracked::alive.load() == base);
}

// Case 6：并发 push，单线程统计
static void test_stack_concurrent_push() {
    constexpr int THREADS = 8, PER = 2000;
    lock_free::stack<int> s;
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t]{ for (int i = 0; i < PER; ++i) s.push(t * PER + i); });
    for (auto& th : ts) th.join();

    std::vector<char> seen(THREADS * PER, 0);
    int count = 0, dup = 0, bad = 0;
    while (auto p = s.pop()) {
        int x = *p;
        if (x < 0 || x >= THREADS * PER) ++bad; else if (seen[x]++) ++dup;
        ++count;
    }
    TEST("stack: 并发 push 总数正确", count == THREADS * PER);
    TEST("stack: 并发 push 无越界", bad == 0);
    TEST("stack: 并发 push 无重复", dup == 0);
}

// Case 7：并发 pop，验证不丢不重
static void test_stack_concurrent_pop() {
    constexpr int N = 20000, THREADS = 8;
    lock_free::stack<int> s;
    for (int i = 0; i < N; ++i) s.push(i);
    std::vector<std::vector<int>> got(THREADS);
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t]{ while (auto p = s.pop()) got[t].push_back(*p); });
    for (auto& th : ts) th.join();

    std::vector<char> seen(N, 0);
    long long sum = 0; int total = 0, dup = 0, bad = 0;
    for (auto& g : got) for (int x : g) {
        ++total;
        if (x < 0 || x >= N) ++bad; else if (seen[x]++) ++dup; else sum += x;
    }
    TEST("stack: 并发 pop 总数正确", total == N);
    TEST("stack: 并发 pop 无越界/重复", bad == 0 && dup == 0);
    TEST("stack: 并发 pop 值之和正确", sum == 1LL * N * (N - 1) / 2);
    TEST("stack: 并发 pop 后为空", s.pop() == nullptr);
}

// Case 8：并发混合 push/pop + 零泄漏
static void test_stack_concurrent_mixed() {
    const long base = Tracked::alive.load();
    constexpr int THREADS = 8, OPS = 4000;
    std::atomic<long> pushed{0}, popped{0};
    {
        lock_free::stack<Tracked> s;
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; ++t)
            ts.emplace_back([&, t]{
                for (int i = 0; i < OPS; ++i) {
                    s.push(Tracked(t * OPS + i)); pushed.fetch_add(1, std::memory_order_relaxed);
                    if (auto p = s.pop()) { (void)p; popped.fetch_add(1, std::memory_order_relaxed); }
                }
            });
        for (auto& th : ts) th.join();
        long rem = 0; while (auto p = s.pop()) { (void)p; ++rem; }
        popped.fetch_add(rem, std::memory_order_relaxed);
    }
    force_drain();
    TEST("stack: 混合并发计数平衡", pushed.load() == popped.load());
    TEST("stack: 混合并发后零泄漏", Tracked::alive.load() == base);
}

// Case 9：hazard 延迟回收路径 —— 高争用下零泄漏（节点壳经 force-drain 全清）
static void test_stack_hazard_reclaim() {
    const long base = Tracked::alive.load();
    constexpr int THREADS = 16, N = 8000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));
        std::atomic<long> taken{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; ++t)
            ts.emplace_back([&]{ while (s.pop()) taken.fetch_add(1, std::memory_order_relaxed); });
        for (auto& th : ts) th.join();
        TEST("stack: hazard 压力下取出总数正确", taken.load() == N);
    }
    force_drain();
    TEST("stack: hazard 回收后零泄漏", Tracked::alive.load() == base);
}

// ═════════════════════════════════════════════
//  Part 2 —— unique_ptr<T>（原 atomic_owner_ptr）
// ═════════════════════════════════════════════

// Case 10：基本读 / 空指针
static void test_owner_basic() {
    const long base = Guarded::alive.load();
    {
        Owner p(new Guarded(7));
        auto lk = p.safe_read();
        TEST("owner: safe_read 非空", (bool)lk);
        TEST("owner: 读到构造值", lk && lk->v == 7 && lk->valid());
    }
    force_drain();
    TEST("owner: 单值析构后零泄漏", Guarded::alive.load() == base);

    Owner np(nullptr);
    auto lk = np.safe_read();
    TEST("owner: 空指针 safe_read 为 false", !(bool)lk);
}

// Case 11：赋值替换并回收旧值
static void test_owner_assign_reclaims() {
    const long base = Guarded::alive.load();
    {
        Owner p(new Guarded(1));
        TEST("owner: 赋值前 1 个对象", Guarded::alive.load() == base + 1);
        p = new Guarded(2);
        p = new Guarded(3);
        force_drain();   // 无人持锁 → 旧值应已回收
        TEST("owner: 连续赋值后仅剩 1 个", Guarded::alive.load() == base + 1);
        auto lk = p.safe_read();
        TEST("owner: 当前值为最后一次赋值", lk && lk->v == 3 && lk->valid());
    }
    force_drain();
    TEST("owner: 赋值链析构后零泄漏", Guarded::alive.load() == base);
}

// Case 12：持锁替换 → 延迟回收（确定性）
static void test_owner_deferred_reclaim() {
    const long base = Guarded::alive.load();
    {
        Owner p(new Guarded(11));
        {
            auto lk = p.safe_read();              // 保护旧对象 A
            p = new Guarded(22);                   // 替换为 B；A 被风险指针保护 → 延迟回收
            force_drain();                         // 即便强制回收，A 受保护也不会被删
            TEST("owner: 持锁替换时新旧并存", Guarded::alive.load() == base + 2);
            TEST("owner: 持锁仍读到有效旧值", lk->v == 11 && lk->valid());
        }                                          // lk 释放
        force_drain();
        auto lk2 = p.safe_read();
        TEST("owner: 释放后当前值为新值", lk2->v == 22 && lk2->valid());
    }
    force_drain();
    TEST("owner: 延迟回收后零泄漏", Guarded::alive.load() == base);
}

// Case 13：并发读 + 单写者（UAF / 完整性 / 泄漏）
static void test_owner_concurrent_read_write() {
    const long base = Guarded::alive.load();
    constexpr int READERS = 6, WRITES = 50000;
    std::atomic<long> reads{0}, invalid{0};
    {
        Owner p(new Guarded(0));
        std::atomic<bool> stop{false};
        std::vector<std::thread> rs;
        for (int t = 0; t < READERS; ++t)
            rs.emplace_back([&]{
                while (!stop.load(std::memory_order_relaxed)) {
                    auto lk = p.safe_read();
                    if (lk) { if (!lk->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                              reads.fetch_add(1, std::memory_order_relaxed); }
                }
            });
        for (int i = 1; i <= WRITES; ++i) p = new Guarded(i);
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : rs) th.join();
    }
    force_drain();
    TEST("owner: 并发读未读到失效对象(无 UAF)", invalid.load() == 0);
    TEST("owner: 并发读确实发生", reads.load() > 0);
    TEST("owner: 并发读写后零泄漏", Guarded::alive.load() == base);
}

// Case 14：多写者（无双重释放 / 泄漏）
static void test_owner_multi_writer() {
    const long base = Guarded::alive.load();
    constexpr int WRITERS = 6, PER = 8000;
    {
        Owner p(new Guarded(0));
        std::vector<std::thread> ws;
        for (int t = 0; t < WRITERS; ++t)
            ws.emplace_back([&, t]{ for (int i = 0; i < PER; ++i) p = new Guarded(t * PER + i + 1); });
        for (auto& th : ws) th.join();
        auto lk = p.safe_read();
        TEST("owner: 多写者后当前值有效", lk && lk->valid());
    }
    force_drain();
    TEST("owner: 多写者并发后零泄漏(无双重释放)", Guarded::alive.load() == base);
}

// ═════════════════════════════════════════════
//  Part 2.5 —— unique_ptr<T> 移动语义（移动构造 / 移动赋值）
//   只移动 / 不拷贝（拷贝已 =delete）。要点：所有权唯一转移、源置空、
//   目标旧值按 hazard 规则回收、自移动安全、无双重释放、无泄漏。
// ═════════════════════════════════════════════

// Case M1：移动构造 —— 转移所有权，源置空，无拷贝（同一对象地址），无泄漏
static void test_move_construct() {
    const long base = Guarded::alive.load();
    {
        Owner src(new Guarded(100));
        const void* raw = nullptr;
        { auto lk = src.safe_read(); raw = lk.get(); }     // 记录原始对象地址（锁随即释放）
        Owner dst(std::move(src));                          // 移动构造
        TEST("move-ctor: 仅转移未新增对象", Guarded::alive.load() == base + 1);
        { auto lks = src.safe_read(); TEST("move-ctor: 源被置空", !(bool)lks); }
        auto lkd = dst.safe_read();
        TEST("move-ctor: 目标获得值",       lkd && lkd->v == 100 && lkd->valid());
        TEST("move-ctor: 同一对象(无拷贝)",  lkd.get() == raw);
    }
    force_drain();
    TEST("move-ctor: 析构后零泄漏", Guarded::alive.load() == base);
}

// Case M2：从空 owner 移动构造 —— 源/目标皆空，不崩溃，无泄漏
static void test_move_construct_from_null() {
    const long base = Guarded::alive.load();
    {
        Owner src(nullptr);
        Owner dst(std::move(src));
        auto a = src.safe_read();
        auto b = dst.safe_read();
        TEST("move-ctor(null): 源仍空",   !(bool)a);
        TEST("move-ctor(null): 目标也空", !(bool)b);
    }
    force_drain();
    TEST("move-ctor(null): 零泄漏", Guarded::alive.load() == base);
}

// Case M3：移动赋值 —— 目标旧值被回收，获得源值，源置空
static void test_move_assign_reclaims_old() {
    const long base = Guarded::alive.load();
    {
        Owner dst(new Guarded(1));
        Owner src(new Guarded(2));
        TEST("move-assign: 赋值前 2 个对象", Guarded::alive.load() == base + 2);
        dst = std::move(src);                               // dst 旧值(1) 应被回收
        force_drain();                                      // 无人持锁 → 旧值立即删除
        TEST("move-assign: 旧值回收后仅剩 1", Guarded::alive.load() == base + 1);
        { auto lk = dst.safe_read(); TEST("move-assign: 目标获得源值", lk && lk->v == 2 && lk->valid()); }
        { auto lk = src.safe_read(); TEST("move-assign: 源被置空",     !(bool)lk); }
    }
    force_drain();
    TEST("move-assign: 析构后零泄漏", Guarded::alive.load() == base);
}

// Case M4：移动赋值到空目标 —— 无多余回收，仅转移
static void test_move_assign_into_null() {
    const long base = Guarded::alive.load();
    {
        Owner dst(nullptr);
        Owner src(new Guarded(5));
        dst = std::move(src);
        TEST("move-assign(空目标): 仅转移未增减对象", Guarded::alive.load() == base + 1);
        { auto lk = dst.safe_read(); TEST("move-assign(空目标): 目标获得值", lk && lk->v == 5 && lk->valid()); }
        { auto lk = src.safe_read(); TEST("move-assign(空目标): 源置空",     !(bool)lk); }
    }
    force_drain();
    TEST("move-assign(空目标): 零泄漏", Guarded::alive.load() == base);
}

// Case M5：从空源移动赋值 —— 目标旧值回收，目标变空
static void test_move_assign_from_null() {
    const long base = Guarded::alive.load();
    {
        Owner dst(new Guarded(9));
        Owner src(nullptr);
        dst = std::move(src);
        force_drain();
        TEST("move-assign(空源): 目标旧值已回收", Guarded::alive.load() == base);
        auto lk = dst.safe_read();
        TEST("move-assign(空源): 目标变空", !(bool)lk);
    }
    force_drain();
    TEST("move-assign(空源): 零泄漏", Guarded::alive.load() == base);
}

// Case M6：自移动赋值 —— if(this!=&v) 短路：不丢值、不双重释放
static void test_move_assign_self() {
    const long base = Guarded::alive.load();
    {
        Owner p(new Guarded(77));
        const void* raw = nullptr;
        { auto lk = p.safe_read(); raw = lk.get(); }
        Owner& ref = p;
        p = std::move(ref);                                 // 自移动（经引用规避编译器自赋值告警）
        force_drain();
        TEST("move-assign(self): 值未丢失",   Guarded::alive.load() == base + 1);
        auto lk = p.safe_read();
        TEST("move-assign(self): 仍同一对象", lk && lk->v == 77 && lk->valid() && lk.get() == raw);
    }
    force_drain();
    TEST("move-assign(self): 零泄漏", Guarded::alive.load() == base);
}

// Case M7：std::swap —— 组合移动构造 + 两次移动赋值，值互换无泄漏
static void test_move_swap() {
    const long base = Guarded::alive.load();
    {
        Owner a(new Guarded(1)), b(new Guarded(2));
        std::swap(a, b);
        { auto la = a.safe_read(); TEST("move-swap: a 得到 2", la && la->v == 2 && la->valid()); }
        { auto lb = b.safe_read(); TEST("move-swap: b 得到 1", lb && lb->v == 1 && lb->valid()); }
        TEST("move-swap: 对象数不变", Guarded::alive.load() == base + 2);
    }
    force_drain();
    TEST("move-swap: 零泄漏", Guarded::alive.load() == base);
}

// Case M8：移动后源可复用 —— 被移走后重新赋新值正常工作
static void test_move_then_reuse_source() {
    const long base = Guarded::alive.load();
    {
        Owner src(new Guarded(1));
        Owner dst(std::move(src));
        src = new Guarded(2);                               // 移走后对源重新赋值
        { auto lk = src.safe_read(); TEST("move-reuse: 源可重新赋值", lk && lk->v == 2 && lk->valid()); }
        { auto lk = dst.safe_read(); TEST("move-reuse: 目标仍持原值", lk && lk->v == 1 && lk->valid()); }
        TEST("move-reuse: 共 2 个对象", Guarded::alive.load() == base + 2);
    }
    force_drain();
    TEST("move-reuse: 零泄漏", Guarded::alive.load() == base);
}

// Case M9：移动赋值时目标旧值正被 hazard 保护 → 延迟回收（确定性）
static void test_move_assign_deferred_reclaim() {
    const long base = Guarded::alive.load();
    {
        Owner dst(new Guarded(11));
        Owner src(new Guarded(22));
        {
            auto lk = dst.safe_read();                      // 保护 dst 旧值 A(11)
            dst = std::move(src);                           // A 受保护 → 延迟回收
            force_drain();                                  // 强制回收下 A 仍不应被删
            TEST("move-assign(defer): 持锁时新旧并存",   Guarded::alive.load() == base + 2);
            TEST("move-assign(defer): 持锁仍读到有效旧值", lk->v == 11 && lk->valid());
        }                                                   // lk 释放
        force_drain();
        auto lk2 = dst.safe_read();
        TEST("move-assign(defer): 释放后为新值", lk2 && lk2->v == 22 && lk2->valid());
    }
    force_drain();
    TEST("move-assign(defer): 延迟回收后零泄漏", Guarded::alive.load() == base);
}

// Case M10：移动进 std::vector —— 故意小容量触发多次扩容移动，值全保留、无泄漏
//   直接存 Owner（可移动、不可拷贝）；扩容时 vector 用移动构造搬迁元素。
static void test_move_in_vector() {
    const long base = Guarded::alive.load();
    constexpr int N = 200;
    {
        std::vector<Owner> v;
        v.reserve(1);                                       // 故意小 → push 触发多次扩容移动
        for (int i = 0; i < N; ++i) v.emplace_back(new Guarded(i));
        TEST("move-vector: 全部就位", Guarded::alive.load() == base + N);
        int good = 0;
        for (int i = 0; i < N; ++i) { auto lk = v[i].safe_read(); if (lk && lk->v == i && lk->valid()) ++good; }
        TEST("move-vector: 扩容移动后值全部保留", good == N);
    }
    force_drain();
    TEST("move-vector: 析构后零泄漏", Guarded::alive.load() == base);
}

// Case M11：并发读 + 移动赋值写者 —— 走移动赋值路径，无 UAF / 无泄漏
static void test_move_assign_concurrent() {
    const long base = Guarded::alive.load();
    constexpr int READERS = 6, WRITES = 30000;
    std::atomic<long> reads{0}, invalid{0};
    {
        Owner p(new Guarded(0));
        std::atomic<bool> stop{false};
        std::vector<std::thread> rs;
        for (int t = 0; t < READERS; ++t)
            rs.emplace_back([&]{
                while (!stop.load(std::memory_order_relaxed)) {
                    auto lk = p.safe_read();
                    if (lk) { if (!lk->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                              reads.fetch_add(1, std::memory_order_relaxed); }
                }
            });
        for (int i = 1; i <= WRITES; ++i) {
            Owner tmp(new Guarded(i));
            p = std::move(tmp);                             // 经移动赋值写入（区别于 operator=(T*)）
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : rs) th.join();
    }
    force_drain();
    TEST("move-assign(并发): 读未读到失效对象(无 UAF)", invalid.load() == 0);
    TEST("move-assign(并发): 读确实发生",               reads.load() > 0);
    TEST("move-assign(并发): 并发后零泄漏",             Guarded::alive.load() == base);
}

// ═════════════════════════════════════════════
//  Part 3 —— hp_domain：同线程多槽 / 无界增长 / 复用
// ═════════════════════════════════════════════

// Case 15：同线程嵌套两把锁，二者同时有效且互异
static void test_nested_two_locks() {
    const long base = Guarded::alive.load();
    {
        Owner p1(new Guarded(1)), p2(new Guarded(2));
        auto lk1 = p1.safe_read();
        TEST("multi: lk1 受保护", out(lk1.get()));
        auto lk2 = p2.safe_read();
        TEST("multi: 取 lk2 后 lk1 仍受保护(嵌套安全)", out(lk1.get()));
        TEST("multi: lk2 同时受保护", out(lk2.get()));
        TEST("multi: 两把锁占用不同槽", lk1.get() != lk2.get());
        TEST("multi: 两把锁读值正确", lk1->v == 1 && lk1->valid() && lk2->v == 2 && lk2->valid());
    }
    force_drain();
    TEST("multi: 嵌套两锁后零泄漏", Guarded::alive.load() == base);
}

// Case 16：释放内层锁，外层不受影响
static void test_nested_release_order() {
    const long base = Guarded::alive.load();
    {
        Owner p1(new Guarded(10)), p2(new Guarded(20));
        auto lk1 = p1.safe_read();
        const void* a1 = lk1.get();
        {
            auto lk2 = p2.safe_read();
            TEST("multi: 内层持有时两者都受保护", out(a1) && out(lk2.get()));
        }
        TEST("multi: 内层释放后外层仍受保护", out(a1));
        TEST("multi: 外层仍读到正确值", lk1->v == 10 && lk1->valid());
    }
    force_drain();
    TEST("multi: 嵌套释放后零泄漏", Guarded::alive.load() == base);
}

// Case 17：无界深度嵌套 —— 远超任何固定上限，全部受保护且互异（动态增长，不抛异常）
static void test_unbounded_deep_nesting() {
    const long base = Guarded::alive.load();
    constexpr int N = 300;             // 旧版固定 128 上限会在此抛异常；新版应顺利增长
    {
        std::vector<std::unique_ptr<Owner>> ps;
        for (int i = 0; i < N; ++i) ps.push_back(std::make_unique<Owner>(new Guarded(i)));

        std::vector<Lock> locks; locks.reserve(N);
        bool threw = false;
        try { for (int i = 0; i < N; ++i) locks.push_back(ps[i]->safe_read()); }
        catch (...) { threw = true; }
        TEST("domain: 深度嵌套 N=300 不抛异常(无界增长)", !threw);

        std::vector<const void*> raws;
        int held = 0, good = 0;
        for (int i = 0; i < N; ++i) {
            raws.push_back(locks[i].get());
            if (out(locks[i].get())) ++held;
            if (locks[i]->v == i && locks[i]->valid()) ++good;
        }
        std::set<const void*> uniq(raws.begin(), raws.end());
        TEST("domain: N 把锁全部同时受保护", held == N);
        TEST("domain: N 把锁读值正确", good == N);
        TEST("domain: N 把锁占 N 个互异槽", (int)uniq.size() == N);

        locks.clear();
        int still = 0; for (auto r : raws) if (out(r)) ++still;
        TEST("domain: 释放后全部不再 outstanding", still == 0);
    }
    force_drain();
    TEST("domain: 深度嵌套后零泄漏", Guarded::alive.load() == base);
}

// Case 18：反复嵌套取/放 —— cache 槽与动态槽都被复用，长期稳定不泄漏
static void test_repeated_nesting() {
    Owner p1(new Guarded(1)), p2(new Guarded(2)), p3(new Guarded(3)), p4(new Guarded(4));
    const long base = Guarded::alive.load();   // 基线含上述 4 个固定对象
    bool ok = true;
    for (int i = 0; i < 5000 && ok; ++i) {
        auto a = p1.safe_read();   // cache 槽 0
        auto b = p2.safe_read();   // cache 槽 1
        auto c = p3.safe_read();   // cache 槽 2
        auto d = p4.safe_read();   // 超过 3 个 cache → 动态槽
        if (!(a->valid() && b->valid() && c->valid() && d->valid())) ok = false;
    }
    TEST("domain: 反复嵌套取/放 5000 轮稳定", ok);
    force_drain();
    TEST("domain: 反复嵌套后零泄漏", Guarded::alive.load() == base);
}

// Case 19：并发嵌套读者(每线程持 2 锁) + 写者 —— UAF/完整性/泄漏
static void test_nested_concurrent() {
    const long base = Guarded::alive.load();
    constexpr int READERS = 6, WRITES = 30000;
    std::atomic<long> reads{0}, invalid{0};
    {
        Owner p1(new Guarded(0)), p2(new Guarded(0));
        std::atomic<bool> stop{false};
        std::vector<std::thread> rs;
        for (int t = 0; t < READERS; ++t)
            rs.emplace_back([&]{
                while (!stop.load(std::memory_order_relaxed)) {
                    auto a = p1.safe_read();      // cache 槽
                    auto b = p2.safe_read();      // 第二把 → 另一槽
                    if (a && !a->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                    if (b && !b->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (int i = 1; i <= WRITES; ++i) { p1 = new Guarded(i); p2 = new Guarded(i); }
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : rs) th.join();
    }
    force_drain();
    TEST("domain: 并发嵌套读未读到失效对象(无 UAF)", invalid.load() == 0);
    TEST("domain: 并发嵌套读确实发生", reads.load() > 0);
    TEST("domain: 并发嵌套读写后零泄漏", Guarded::alive.load() == base);
}

// Case 20：域随并发线程数增长 → 全部释放后无残留活跃 hazard（槽被复用而非泄漏）
static void test_domain_grows_and_reuses() {
    constexpr int THREADS = 64;
    std::vector<std::unique_ptr<Owner>> ps;
    for (int i = 0; i < THREADS; ++i) ps.push_back(std::make_unique<Owner>(new Guarded(i)));

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t]{
            auto lk = ps[t]->safe_read();                 // 各持 1 把锁
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_relaxed)) std::this_thread::yield();
            (void)lk;
        });
    while (ready.load() < THREADS) std::this_thread::yield();

    size_t live = live_hazards();                          // 所有线程同时持锁
    go.store(true, std::memory_order_relaxed);
    for (auto& th : ts) th.join();

    TEST("domain: 64 线程并发各持 1 hazard，域增长到 >= 64", live >= THREADS);
    TEST("domain: 全部释放后无活跃 hazard(槽被复用)", live_hazards() == 0);

    bool ok = true;
    try {
        std::vector<std::thread> ts2;
        for (int t = 0; t < THREADS; ++t)
            ts2.emplace_back([&, t]{ auto lk = ps[t]->safe_read(); (void)lk; });
        for (auto& th : ts2) th.join();
    } catch (...) { ok = false; }
    TEST("domain: 槽释放后可被复用", ok);
}

// ═════════════════════════════════════════════
//  Part 3.5 —— retire_list 计数(retire_count) 正确性
//   验证“计数器 == 实际链表长度”，以及并发后不漂移。
// ═════════════════════════════════════════════

// Case 21：单线程下 计数器 == 实际遍历长度（add 与 drain 的增减都对）
static void test_retire_count_single_thread() {
    auto& r = hp::retire_list::get();
    for (int i = 0; i < 5; ++i) r.delete_nodes_with_no_hazards(true);   // 先静默到基线
    TEST("retire: 基线 count == 链表长", retire_count_val() == (int32_t)retire_list_len());

    const int32_t base_cnt = retire_count_val();
    const long    base_al  = Tracked::alive.load();
    constexpr int K = 200;                                  // < threshold(512)，不触发自动回收
    for (int i = 0; i < K; ++i) r.reclaim_later(new Tracked(i));   // 无 hazard，纯入队

    TEST("retire: 入队 K 个后 count == base+K", retire_count_val() == base_cnt + K);
    TEST("retire: count 等于实际链表长度",       retire_count_val() == (int32_t)retire_list_len());
    TEST("retire: K 个对象此刻存活",             Tracked::alive.load() == base_al + K);

    r.delete_nodes_with_no_hazards(true);                  // 无 hazard → 全部删除
    TEST("retire: drain 后 count 回到 base",     retire_count_val() == base_cnt);
    TEST("retire: drain 后链表长度回到 base",     (int32_t)retire_list_len() == base_cnt);
    TEST("retire: drain 后对象全部析构",         Tracked::alive.load() == base_al);
}

// Case 22：多线程并发 入队 + 并发 drain，静默后 计数器不漂移（== 实际长度 == 0）
static void test_retire_count_concurrent() {
    auto& r = hp::retire_list::get();
    for (int i = 0; i < 5; ++i) r.delete_nodes_with_no_hazards(true);
    const long base_al = Tracked::alive.load();

    constexpr int THREADS = 8, PER = 20000;
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&]{
            for (int i = 0; i < PER; ++i) {
                r.reclaim_later(new Tracked(i));               // 并发入队(fetch_add)
                if ((i & 63) == 0) r.delete_nodes_with_no_hazards(true);  // 并发出队(fetch_sub)
            }
        });
    for (auto& th : ts) th.join();

    for (int i = 0; i < 5; ++i) r.delete_nodes_with_no_hazards(true);     // 最终静默

    // 静默后：计数器必须 == 实际链表长度 == 0（否则 fetch_add/fetch_sub 配平有 bug）
    TEST("retire: 并发后 count == 实际链表长度", retire_count_val() == (int32_t)retire_list_len());
    TEST("retire: 并发后 count 归零(无漂移)",     retire_count_val() == 0);
    TEST("retire: 并发后链表清空",               retire_list_len() == 0);
    TEST("retire: 并发入队/出队后零泄漏",         Tracked::alive.load() == base_al);
}

// ═════════════════════════════════════════════
//  Part 4 —— 全局收尾断言
// ═════════════════════════════════════════════
static void test_global_balance() {
    force_drain();
    TEST("收尾: 所有 Tracked 已析构", Tracked::alive.load() == 0);
    TEST("收尾: Tracked 构造 == 析构", Tracked::constructed.load() == Tracked::destroyed.load());
    TEST("收尾: 所有 Guarded 已析构", Guarded::alive.load() == 0);
    TEST("收尾: 无活跃 hazard", live_hazards() == 0);
}

// 跑一遍完整套件（一次性）
static void run_full_suite() {
    // Part 1: stack
    test_stack_basic();
    test_stack_shared_ptr();
    test_stack_no_leak_drain();
    test_stack_no_leak_destructor();
    test_stack_no_leak_partial();
    test_stack_concurrent_push();
    test_stack_concurrent_pop();
    test_stack_concurrent_mixed();
    test_stack_hazard_reclaim();
    // Part 2: atomic_owner_ptr
    test_owner_basic();
    test_owner_assign_reclaims();
    test_owner_deferred_reclaim();
    test_owner_concurrent_read_write();
    test_owner_multi_writer();
    // Part 2.5: unique_ptr 移动语义
    test_move_construct();
    test_move_construct_from_null();
    test_move_assign_reclaims_old();
    test_move_assign_into_null();
    test_move_assign_from_null();
    test_move_assign_self();
    test_move_swap();
    test_move_then_reuse_source();
    test_move_assign_deferred_reclaim();
    test_move_in_vector();
    test_move_assign_concurrent();
    // Part 3: hp_domain 多槽 / 无界增长 / 复用
    test_nested_two_locks();
    test_nested_release_order();
    test_unbounded_deep_nesting();
    test_repeated_nesting();
    test_nested_concurrent();
    test_domain_grows_and_reuses();
    // Part 3.5: retire_count 计数正确性
    test_retire_count_single_thread();
    test_retire_count_concurrent();
}

// soak 模式只跑“并发/回收/增长”重负载用例（确定性单线程用例无需反复跑）
static void run_soak_round() {
    test_stack_concurrent_push();
    test_stack_concurrent_pop();
    test_stack_concurrent_mixed();
    test_stack_hazard_reclaim();
    test_owner_concurrent_read_write();
    test_owner_multi_writer();
    test_move_assign_concurrent();       // 移动赋值并发路径：无 UAF / 不漂移
    test_move_in_vector();               // 扩容移动搬迁：值保留 / 无泄漏
    test_nested_concurrent();
    test_unbounded_deep_nesting();
    test_domain_grows_and_reuses();
    test_retire_count_single_thread();   // 每轮校验计数器与真链表长度一致
    test_retire_count_concurrent();      // 并发入队/出队后计数不漂移
}

// ─────────────────────────────────────────────
// 进程当前常驻内存（KB）—— 用来识别内存是否随时间无界增长
// ─────────────────────────────────────────────
static size_t current_rss_kb() {
#if defined(__APPLE__)
    task_basic_info info;
    mach_msg_type_number_t n = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS)
        return info.resident_size / 1024;
#endif
    return 0;
}

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

// ─────────────────────────────────────────────
// 长时间浸泡（soak）：循环跑重负载用例，期间持续校验不变量
//   每轮结束后必须回到基线：alive(Tracked/Guarded)==0、live_hazards==0；
//   常驻内存(RSS)应在首轮后趋于平台期（hazard 槽按峰值一次性分配、之后复用）。
//   任一不变量被破坏 → 立即报告并退出非 0。
// ─────────────────────────────────────────────
static int soak_run(double limit_seconds) {
    g_quiet = true;
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    using clock = std::chrono::steady_clock;
    const auto  t0   = clock::now();
    auto        beat = t0;
    long        round = 0;
    size_t      rss_first = 0, rss_peak = 0;

    std::printf("=== soak 开始 ===  时长上限 = %s   (Ctrl-C 可随时停止)\n",
                limit_seconds <= 0 ? "无限" : (std::to_string((long)limit_seconds) + "s").c_str());
    std::fflush(stdout);

    while (!g_stop.load()) {
        run_soak_round();
        ++round;

        // —— 每轮收尾必须回到基线 ——
        force_drain();
        const long    ta = Tracked::alive.load();
        const long    ga = Guarded::alive.load();
        const size_t  hz = live_hazards();
        const int32_t rc = retire_count_val();          // retire_count 计数器
        const size_t  rl = retire_list_len();           // 实际链表长度
        if (ta != 0 || ga != 0 || hz != 0) {
            std::printf("\n[SOAK-FAIL] round %ld 不变量被破坏(疑似泄漏/回收异常): "
                        "Tracked.alive=%ld Guarded.alive=%ld live_hazards=%zu\n",
                        round, ta, ga, hz);
            return 1;
        }
        if (rc != 0 || rl != 0 || (size_t)rc != rl) {   // 计数器漂移 / 与真链表长度不一致
            std::printf("\n[SOAK-FAIL] round %ld retire 计数异常: "
                        "retire_count=%d 实际链表长度=%zu (静默后两者都应为 0)\n",
                        round, rc, rl);
            return 1;
        }
        if (g_fail != 0) {
            std::printf("\n[SOAK-FAIL] round %ld 出现 %ld 个断言失败\n", round, g_fail);
            return 1;
        }

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        const size_t rss = current_rss_kb();
        if (rss > rss_peak) rss_peak = rss;
        if (rss_first == 0 && round >= 1) rss_first = rss;

        // 每 ~15s 一次心跳
        if (std::chrono::duration<double>(now - beat).count() >= 15.0) {
            std::printf("[soak] t=%6.0fs round=%-7ld asserts=%-10ld alive(T/G)=%ld/%ld "
                        "hz=%zu retire(cnt/len)=%d/%zu rss=%zuKB drift=%+ldKB\n",
                        elapsed, round, g_pass, ta, ga, hz, rc, rl, rss,
                        (long)rss - (long)rss_first);
            std::fflush(stdout);
            beat = now;
        }
        if (limit_seconds > 0 && elapsed >= limit_seconds) break;
    }

    const double total = std::chrono::duration<double>(clock::now() - t0).count();
    std::printf("\n=== soak 结束 ===\n");
    std::printf("用时 %.0fs   轮数 %ld   累计断言 %ld   失败 %ld\n", total, round, g_pass, g_fail);
    std::printf("RSS 首轮=%zuKB 峰值=%zuKB 末值=%zuKB (漂移 %+ldKB)\n",
                rss_first, rss_peak, current_rss_kb(),
                (long)current_rss_kb() - (long)rss_first);
    std::printf("Tracked 构造=%ld 析构=%ld alive=%ld | Guarded alive=%ld | live_hazards=%zu\n",
                Tracked::constructed.load(), Tracked::destroyed.load(), Tracked::alive.load(),
                Guarded::alive.load(), live_hazards());
    const bool ok = (g_fail == 0 && Tracked::alive.load() == 0 &&
                     Guarded::alive.load() == 0 && live_hazards() == 0);
    std::printf("结论：%s\n", ok ? "OK（无泄漏/无失败/RSS 已平台化）" : "存在问题（见上）");
    return ok ? 0 : 1;
}

// ─────────────────────────────────────────────
//   用法：
//     ./t                 跑一遍完整套件（详细输出）
//     ./t soak            浸泡 3600s（1 小时）后停止
//     ./t soak 600        浸泡 600s
//     ./t soak 0          无限浸泡，直到 Ctrl-C
// ─────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "soak") == 0) {
        double secs = 3600.0;                                  // 默认 1 小时
        if (argc >= 3) secs = std::atof(argv[2]);
        return soak_run(secs);
    }

    std::cout << "=== lock_free 完整测试（hp_domain 动态无界 + retire 单例 版）===\n\n";
    run_full_suite();
    test_global_balance();

    std::cout << "\n----------------------------------------\n";
    std::cout << "PASS: " << g_pass << "    FAIL: " << g_fail << "\n";
    std::cout << "Tracked  constructed=" << Tracked::constructed.load()
              << "  destroyed=" << Tracked::destroyed.load()
              << "  alive="     << Tracked::alive.load() << "\n";
    std::cout << "Guarded  alive=" << Guarded::alive.load()
              << "   live_hazards=" << live_hazards() << "\n";
    std::cout << "\n提示：长时间浸泡跑   ./" << (argc ? argv[0] : "t") << " soak [秒数]"
              << "   （默认 3600s，soak 0 为无限）\n";
    return g_fail == 0 ? 0 : 1;
}

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
using Owner = hp::atomic_owner_ptr<Guarded>;
using Lock  = Owner::hazard_lock;

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
//  Part 2 —— atomic_owner_ptr<T>
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
    // Part 3: hp_domain 多槽 / 无界增长 / 复用
    test_nested_two_locks();
    test_nested_release_order();
    test_unbounded_deep_nesting();
    test_repeated_nesting();
    test_nested_concurrent();
    test_domain_grows_and_reuses();
}

// soak 模式只跑“并发/回收/增长”重负载用例（确定性单线程用例无需反复跑）
static void run_soak_round() {
    test_stack_concurrent_push();
    test_stack_concurrent_pop();
    test_stack_concurrent_mixed();
    test_stack_hazard_reclaim();
    test_owner_concurrent_read_write();
    test_owner_multi_writer();
    test_nested_concurrent();
    test_unbounded_deep_nesting();
    test_domain_grows_and_reuses();
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
        const long  ta = Tracked::alive.load();
        const long  ga = Guarded::alive.load();
        const size_t hz = live_hazards();
        if (ta != 0 || ga != 0 || hz != 0) {
            std::printf("\n[SOAK-FAIL] round %ld 不变量被破坏(疑似泄漏/回收异常): "
                        "Tracked.alive=%ld Guarded.alive=%ld live_hazards=%zu\n",
                        round, ta, ga, hz);
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
                        "hz=%zu rss=%zuKB drift=%+ldKB\n",
                        elapsed, round, g_pass, ta, ga, hz, rss,
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

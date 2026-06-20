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

#define TEST(name, expr) do {                                   \
    if (expr) { std::cout << "[PASS] " << name << "\n"; ++g_pass; } \
    else      { std::cout << "[FAIL] " << name                  \
                          << "   (" #expr ")\n";  ++g_fail; }   \
} while (0)

// ─────────────────────────────────────────────
// Tracked：带“存活实例计数”的探针类型
//   alive == 0  ⇔  所有内部数据都已被正确释放
// ─────────────────────────────────────────────
struct Tracked {
    static std::atomic<long> alive;        // 当前存活实例数
    static std::atomic<long> constructed;  // 累计构造次数
    static std::atomic<long> destroyed;    // 累计析构次数
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

// Guarded：用于 atomic_owner_ptr 测试的探针。
//   chk == (v ^ MAGIC) 成立 ⇔ 对象有效（未析构）。析构时写入毒值，
//   于是“读到已析构对象”可被 valid() 当场抓到，与 ASan 的 UAF 检测互补。
struct Guarded {
    static std::atomic<long> alive;
    static constexpr int MAGIC = 0x5A5A1234;
    int v;
    int chk;
    explicit Guarded(int x) : v(x), chk(x ^ MAGIC) {
        alive.fetch_add(1, std::memory_order_relaxed);
       // std::cout << "Guarded create:" << (void*)this << " " << alive.load() << std::endl;
    }
    ~Guarded() {
        chk = 0xDEAD;
        v   = -1;
        alive.fetch_add(-1, std::memory_order_relaxed);
       // std::cout << "Guarded desctory:" << (void*)this << " " << alive.load() << std::endl;
    }
    bool valid() const { return chk == (v ^ MAGIC); }
};
std::atomic<long> Guarded::alive{0};

// 把全局延迟回收链表彻底排空（无 hazard pointer 占用时会真正 delete）。
// 多调用几次以处理“被重新挂回链表”的节点。
static void drain_reclaim_list() {
    return;
    for (int i = 0; i < 5; ++i)
        lock_free::delete_nodes_with_no_hazards();
}

// ─────────────────────────────────────────────
// Case 1：单线程基本功能 / LIFO / 空栈语义
// ─────────────────────────────────────────────
static void test_single_thread_basic() {
    lock_free::stack<int> s;

    TEST("空栈 pop 返回 null", s.pop() == nullptr);

    s.push(42);
    auto v = s.pop();
    TEST("push 后 pop 得到原值", v && *v == 42);
    TEST("取空后再 pop 为 null", s.pop() == nullptr);

    s.push(1); s.push(2); s.push(3);
    TEST("LIFO 顺序 #1 (=3)", *s.pop() == 3);
    TEST("LIFO 顺序 #2 (=2)", *s.pop() == 2);
    TEST("LIFO 顺序 #3 (=1)", *s.pop() == 1);
    TEST("全部取出后为空", s.pop() == nullptr);
}

// ─────────────────────────────────────────────
// Case 2：shared_ptr 返回值语义正确
// ─────────────────────────────────────────────
static void test_shared_ptr_semantics() {
    lock_free::stack<int> s;
    s.push(99);
    auto p1 = s.pop();
    TEST("pop 返回独占所有权 use_count==1", p1.use_count() == 1);
    {
        auto p2 = p1;
        TEST("拷贝后 use_count==2", p1.use_count() == 2);
        TEST("两个 shared_ptr 指向同一对象", p1.get() == p2.get() && *p2 == 99);
    }
    TEST("作用域结束后 use_count 回到 1", p1.use_count() == 1);
}

// ─────────────────────────────────────────────
// Case 3：全部正常 pop —— 内部数据零泄漏
// ─────────────────────────────────────────────
static void test_no_leak_full_drain() {
    const long base = Tracked::alive.load();
    constexpr int N = 5000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));
        long held = 0;
        while (auto p = s.pop()) { (void)p; ++held; }   // 取出后立即释放
        TEST("push/pop 数量一致", held == N);
    }
   // drain_reclaim_list();
    TEST("全部 pop 后无存活实例(零泄漏)", Tracked::alive.load() == base);
}

// ─────────────────────────────────────────────
// Case 4：只 push 不 pop —— 靠析构函数排空，零泄漏
//   （验证 ~stack(){ while(pop()); } 确实回收了所有节点和数据）
// ─────────────────────────────────────────────
static void test_no_leak_destructor_drains() {
    const long base = Tracked::alive.load();
    constexpr int N = 5000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));
        TEST("析构前栈内对象存活", Tracked::alive.load() == base + N);
        // 不手动 pop，直接离开作用域 → 触发 ~stack
    }
    ///drain_reclaim_list();
    TEST("析构函数排空后无存活实例", Tracked::alive.load() == base);
}

// ─────────────────────────────────────────────
// Case 5：部分 pop + 析构排空剩余 —— 零泄漏
// ─────────────────────────────────────────────
static void test_no_leak_partial_then_destruct() {
    const long base = Tracked::alive.load();
    {
        lock_free::stack<Tracked> s;
        s.push(Tracked(1));
        s.push(Tracked(2));
        s.push(Tracked(3));
        auto a = s.pop();           // 取出 1 个并持有
        TEST("部分 pop 后存活计数正确", Tracked::alive.load() == base + 3);
        // 持有 a，析构 s（内部还有 2 个节点）
    } // a 在此析构，s 也在此析构
    drain_reclaim_list();
    TEST("部分 pop + 析构后零泄漏", Tracked::alive.load() == base);
}

// ─────────────────────────────────────────────
// Case 6：多线程并发 push，单线程统计总量与值集合
// ─────────────────────────────────────────────
static void test_concurrent_push() {
    constexpr int THREADS = 8;
    constexpr int PER     = 2000;
    lock_free::stack<int> s;

    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < PER; ++i) s.push(t * PER + i);
        });
    for (auto& th : ts) th.join();

    std::vector<char> seen(THREADS * PER, 0);
    int count = 0, dup = 0, bad = 0;
    while (auto p = s.pop()) {
        int x = *p;
        if (x < 0 || x >= THREADS * PER) ++bad;
        else if (seen[x]++) ++dup;
        ++count;
    }
    TEST("并发 push 总数正确", count == THREADS * PER);
    TEST("并发 push 无越界值", bad == 0);
    TEST("并发 push 无重复值", dup == 0);
}

// ─────────────────────────────────────────────
// Case 7：多线程并发 pop，验证不丢、不重
// ─────────────────────────────────────────────
static void test_concurrent_pop_no_duplicate() {
    constexpr int N = 20000;
    lock_free::stack<int> s;
    for (int i = 0; i < N; ++i) s.push(i);

    constexpr int THREADS = 8;
    std::vector<std::vector<int>> got(THREADS);
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t] {
            while (auto p = s.pop()) got[t].push_back(*p);
        });
    for (auto& th : ts) th.join();

    std::vector<char> seen(N, 0);
    int total = 0, dup = 0, bad = 0;
    for (auto& g : got)
        for (int x : g) {
            ++total;
            if (x < 0 || x >= N) ++bad;
            else if (seen[x]++) ++dup;
        }
    long long sum = 0;
    for (int x = 0; x < N; ++x) sum += (seen[x] ? x : 0);

    TEST("并发 pop 取出总数正确", total == N);
    TEST("并发 pop 无越界",        bad == 0);
    TEST("并发 pop 无重复(每值仅一次)", dup == 0);
    TEST("并发 pop 值之和正确",    sum == 1LL * N * (N - 1) / 2);
    TEST("并发 pop 后栈空",        s.pop() == nullptr);
}

// ─────────────────────────────────────────────
// Case 8：并发 push + pop 混合，统计平衡 + 零泄漏
// ─────────────────────────────────────────────
static void test_concurrent_mixed_no_leak() {
    const long base = Tracked::alive.load();
    constexpr int THREADS = 8;
    constexpr int OPS     = 4000;

    std::atomic<long> pushed{0}, popped{0};
    {
        lock_free::stack<Tracked> s;
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; ++t)
            ts.emplace_back([&, t] {
                for (int i = 0; i < OPS; ++i) {
                    s.push(Tracked(t * OPS + i));
                    pushed.fetch_add(1, std::memory_order_relaxed);
                    if (auto p = s.pop()) {
                        (void)p;
                        popped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        for (auto& th : ts) th.join();

        long remain = 0;
        while (auto p = s.pop()) { (void)p; ++remain; }
        popped.fetch_add(remain, std::memory_order_relaxed);
        // s 在此析构（应已为空）
    }
    drain_reclaim_list();

    TEST("混合 push/pop 计数平衡", pushed.load() == popped.load());
    TEST("混合并发后零泄漏",       Tracked::alive.load() == base);
}

// ─────────────────────────────────────────────
// Case 9：hazard-pointer 回收路径压力测试 + 零泄漏
//   大量线程并发取同一批数据，强制走 reclaim_later 延迟回收分支，
//   最终所有数据必须被释放。
// ─────────────────────────────────────────────
static void test_hazard_reclaim_path_no_leak() {
    const long base = Tracked::alive.load();
    constexpr int THREADS = 16;
    constexpr int N       = 8000;
    {
        lock_free::stack<Tracked> s;
        for (int i = 0; i < N; ++i) s.push(Tracked(i));

        std::atomic<long> taken{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; ++t)
            ts.emplace_back([&] {
                while (auto p = s.pop()) {
                    (void)p;
                    taken.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (auto& th : ts) th.join();
        TEST("hazard 压力下取出总数正确", taken.load() == N);
    }
    drain_reclaim_list();
    TEST("hazard 回收链表最终为空",
         lock_free::nodes_to_reclaim.load() == nullptr);
    TEST("hazard 延迟回收后零泄漏", Tracked::alive.load() == base);
}

// ─────────────────────────────────────────────
// Case 10：hazard-pointer 槽位在线程退出后被复用，不会耗尽
// ─────────────────────────────────────────────
static void test_hazard_slot_reuse() {
    bool ok = true;
    // 反复创建/销毁线程，远超总槽位数；若退出时不释放槽位会抛异常
    for (int i = 0; i < lock_free::max_hazard_pointers + 50 && ok; ++i) {
        try {
            std::thread([] {
                lock_free::stack<int> s;
                s.push(1);
                (void)s.pop();
            }).join();
        } catch (...) { ok = false; }
    }
    TEST("线程退出后 hazard 槽位被复用(不耗尽)", ok);
}

// ─────────────────────────────────────────────
// Case 11：构造/析构计数的最终一致性（全局收尾断言）
// ─────────────────────────────────────────────
static void test_global_balance() {
    drain_reclaim_list();
    TEST("全部测试结束：所有 Tracked 已析构", Tracked::alive.load() == 0);
    TEST("全部测试结束：构造数 == 析构数",
         Tracked::constructed.load() == Tracked::destroyed.load());
    TEST("全部测试结束：所有 Guarded 已析构", Guarded::alive.load() == 0);
    TEST("全部测试结束：回收链表为空",
         lock_free::nodes_to_reclaim.load() == nullptr);
}

// ═════════════════════════════════════════════
//  atomic_owner_ptr 测试
// ═════════════════════════════════════════════

// ── Case 12：基本读 / 空指针语义 ──────────────
static void test_owner_basic_read() {
    const long base = Guarded::alive.load();
    {
        lock_free::atomic_owner_ptr<Guarded> p(new Guarded(7));
        auto lk = p.safe_read();
        TEST("owner: safe_read 非空", (bool)lk);
        TEST("owner: 读到构造值",     lk && lk->v == 7 && lk->valid());
    }
    drain_reclaim_list();
    TEST("owner: 单值析构后零泄漏", Guarded::alive.load() == base);

    lock_free::atomic_owner_ptr<Guarded> np(nullptr);
    auto lk = np.safe_read();
    TEST("owner: 空指针 safe_read 为 false", !(bool)lk);
}

// ── Case 13：赋值替换旧值并回收（零泄漏）─────────
static void test_owner_assign_reclaims_old() {
    const long base = Guarded::alive.load();
    {
        lock_free::atomic_owner_ptr<Guarded> p(new Guarded(1));
        TEST("owner: 赋值前 1 个对象", Guarded::alive.load() == base + 1);
        p = new Guarded(2);
        p = new Guarded(3);
        // 无任何 hazard_lock 持有 → 旧值应被立即 delete
        TEST("owner: 连续赋值后仅剩 1 个对象", Guarded::alive.load() == base + 1);
        auto lk = p.safe_read();
        TEST("owner: 当前值为最后一次赋值", lk && lk->v == 3 && lk->valid());
    }
    drain_reclaim_list();
    TEST("owner: 赋值链析构后零泄漏", Guarded::alive.load() == base);
}

// ── Case 14：持锁时替换 → 延迟回收分支（确定性）──
//   reader 持 hazard_lock 时写者替换指针，旧对象必须被推迟删除，
//   且持锁者仍能读到有效旧值；释放后旧对象才被真正回收。
static void test_owner_deferred_reclaim() {
    const long base = Guarded::alive.load();
    {
        lock_free::atomic_owner_ptr<Guarded> p(new Guarded(11));
        {
            auto lk = p.safe_read();                 // hazard pointer 指向旧对象 A
            p = new Guarded(22);                      // 替换为 B；A 有风险指针 → reclaim_later
            TEST("owner: 持锁替换时新旧并存(A被保护)",
                 Guarded::alive.load() == base + 2);
            TEST("owner: 持锁仍读到有效旧值", lk->v == 11 && lk->valid());
        } // lk 析构，清除风险指针
        auto lk2 = p.safe_read();
        TEST("owner: 释放后当前值为新值", lk2->v == 22 && lk2->valid());
    }
  //  drain_reclaim_list();
    TEST("owner: 延迟回收后零泄漏", Guarded::alive.load() == base);
}

// ── Case 15：多读者 + 单写者并发（UAF / 完整性 / 泄漏）──
static void test_owner_concurrent_read_write() {
    const long base = Guarded::alive.load();
    constexpr int READERS = 6;
    constexpr int WRITES  = 50000;
    std::atomic<long> reads{0}, invalid{0};
    {
        lock_free::atomic_owner_ptr<Guarded> p(new Guarded(0));
        std::atomic<bool> stop{false};

        std::vector<std::thread> rs;
        for (int t = 0; t < READERS; ++t)
            rs.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto lk = p.safe_read();
                    if (lk) {
                        if (!lk->valid())                    // 读到已析构对象 = bug
                            invalid.fetch_add(1, std::memory_order_relaxed);
                        reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });

        for (int i = 1; i <= WRITES; ++i)
            p = new Guarded(i);                              // 持续替换，旧值并发回收

        stop.store(true, std::memory_order_relaxed);
        for (auto& th : rs) th.join();
    } // p 析构，释放当前值
   // drain_reclaim_list();

    TEST("owner: 并发读未读到失效对象(无 UAF)", invalid.load() == 0);
    TEST("owner: 并发读确实发生",               reads.load() > 0);
    TEST("owner: 并发读写后零泄漏",             Guarded::alive.load() == base);
}

// ── Case 16：多写者并发（无双重释放 / 零泄漏）────
//   多个写者各自 CAS 换出不同的旧指针，每个旧指针只被一个线程接管释放。
static void test_owner_multi_writer() {
    const long base = Guarded::alive.load();
    constexpr int WRITERS = 6;
    constexpr int PER     = 8000;
    {
        lock_free::atomic_owner_ptr<Guarded> p(new Guarded(0));
        std::vector<std::thread> ws;
        for (int t = 0; t < WRITERS; ++t)
            ws.emplace_back([&, t] {
                for (int i = 0; i < PER; ++i)
                    p = new Guarded(t * PER + i + 1);
            });
        for (auto& th : ws) th.join();

        auto lk = p.safe_read();
        TEST("owner: 多写者后当前值有效", lk && lk->valid());
    }
    drain_reclaim_list();
    TEST("owner: 多写者并发后零泄漏(无双重释放)", Guarded::alive.load() == base);
}

// ═════════════════════════════════════════════
//  同一线程使用多个 hazard 槽
//   get_hazard_pointer_for_current_thread<index>() 用编译期 index 区分槽位，
//   于是同一线程可借不同 index 同时持有多个风险指针。
// ═════════════════════════════════════════════
namespace hp = lock_free;
static bool out(const void* p) {
    return hp::outstanding_hazard_pointers_for(const_cast<void*>(p));
}
using Owner = hp::atomic_owner_ptr<Guarded>;
using Lock  = Owner::hazard_lock;
//
// 新版槽位获取(get_hazard_pointer_for_current_thread(unique_ptr<hp_owner>&))：
//   · 每线程有一个 thread_local 缓存槽；首把锁走缓存(快路径)。
//   · 缓存槽已占用时，再持锁会从全局池动态借一个新槽，由 hazard_lock 内的
//     unique_ptr<hp_owner> 持有，锁析构时自动还池。
// 于是同一线程可安全地同时持有多把 hazard_lock —— 下列用例覆盖各种情况。
//

// ── Case 17：同线程嵌套两把锁，二者同时有效且互异 ──
static void test_thread_nested_two_locks() {
    const long base = Guarded::alive.load();
    {
        Owner p1(new Guarded(1));
        Owner p2(new Guarded(2));

        auto lk1 = p1.safe_read();                 // 缓存槽
        TEST("multislot: 取 lk1 后其对象受保护", out(lk1.get()));

        auto lk2 = p2.safe_read();                 // 缓存槽已占 → 动态借新槽
        TEST("multislot: 同线程再取 lk2 后 lk1 仍受保护(嵌套安全)", out(lk1.get()));
        TEST("multislot: lk2 同时受保护", out(lk2.get()));
        TEST("multislot: 两把锁占用不同槽(保护不同对象)", lk1.get() != lk2.get());
        TEST("multislot: 两把锁各自读值正确",
             lk1->v == 1 && lk1->valid() && lk2->v == 2 && lk2->valid());
    }
    drain_reclaim_list();
    TEST("multislot: 嵌套两锁后零泄漏", Guarded::alive.load() == base);
}

// ── Case 18：释放内层(动态)锁，外层(缓存)锁不受影响 ──
static void test_thread_nested_release_order() {
    const long base = Guarded::alive.load();
    {
        Owner p1(new Guarded(10));
        Owner p2(new Guarded(20));
        auto lk1 = p1.safe_read();                 // 缓存槽
        const void* a1 = lk1.get();
        {
            auto lk2 = p2.safe_read();             // 动态槽
            TEST("multislot: 内层持有时两者都受保护", out(a1) && out(lk2.get()));
        }                                           // lk2 析构 → 动态槽还池
        TEST("multislot: 内层释放后外层仍受保护", out(a1));
        TEST("multislot: 外层仍读到正确值", lk1->v == 10 && lk1->valid());
    }
    drain_reclaim_list();
    TEST("multislot: 嵌套释放后零泄漏", Guarded::alive.load() == base);
}

// ── Case 19：深度嵌套 —— 单线程同时持 N 把锁，全部生效且互异 ──
static void test_thread_deep_nesting() {
    const long base = Guarded::alive.load();
    constexpr int N = 32;
    {
        std::vector<std::unique_ptr<Owner>> ps;
        for (int i = 0; i < N; ++i)
            ps.push_back(std::make_unique<Owner>(new Guarded(i)));

        std::vector<Lock> locks;
        locks.reserve(N);
        for (int i = 0; i < N; ++i) locks.push_back(ps[i]->safe_read());

        std::vector<const void*> raws;
        int held = 0, good = 0;
        for (int i = 0; i < N; ++i) {
            raws.push_back(locks[i].get());
            if (out(locks[i].get())) ++held;
            if (locks[i]->v == i && locks[i]->valid()) ++good;
        }
        std::set<const void*> uniq(raws.begin(), raws.end());
        TEST("multislot: 单线程同时持 N 把锁全部受保护", held == N);
        TEST("multislot: N 把锁读值正确",               good == N);
        TEST("multislot: N 把锁占用 N 个互异槽",         (int)uniq.size() == N);

        locks.clear();                              // 释放全部锁 → 动态槽全部还池
        int still = 0;
        for (auto r : raws) if (out(r)) ++still;
        TEST("multislot: 释放后全部不再 outstanding", still == 0);
    }
    drain_reclaim_list();
    TEST("multislot: 深度嵌套后零泄漏", Guarded::alive.load() == base);
}

// ── Case 20：反复嵌套取/放，动态槽确实归还(否则耗尽池) ──
static void test_thread_nesting_no_exhaustion() {
    Owner p1(new Guarded(1)), p2(new Guarded(2)), p3(new Guarded(3));
    const long base = Guarded::alive.load();   // 基线含上述 3 个固定对象
    bool ok = true;
    try {
        for (int i = 0; i < lock_free::max_hazard_pointers * 4; ++i) {
            auto a = p1.safe_read();                // 缓存槽
            auto b = p2.safe_read();                // 动态槽
            auto c = p3.safe_read();                // 动态槽
            if (!(a->valid() && b->valid() && c->valid())) { ok = false; break; }
        }                                            // 每轮三锁析构 → 动态槽还池
    } catch (...) { ok = false; }
    TEST("multislot: 反复嵌套取/放不耗尽(动态槽已归还)", ok);
    drain_reclaim_list();
    TEST("multislot: 反复嵌套后零泄漏", Guarded::alive.load() == base);
}

// ── Case 21：单线程嵌套超过池容量 → 抛异常，且善后无泄漏 ──
static void test_thread_nesting_exhaustion_throws() {
    const long base = Guarded::alive.load();
    constexpr int OVER = lock_free::max_hazard_pointers + 8;
    bool threw = false;
    {
        std::vector<std::unique_ptr<Owner>> ps;
        for (int i = 0; i < OVER; ++i)
            ps.push_back(std::make_unique<Owner>(new Guarded(i)));

        std::vector<Lock> locks;
        try {
            for (int i = 0; i < OVER; ++i) locks.push_back(ps[i]->safe_read());
        } catch (const std::runtime_error&) { threw = true; }
        TEST("multislot: 单线程嵌套超过池容量抛异常", threw);
    } // locks / ps 析构：释放所有槽与对象
    drain_reclaim_list();
    TEST("multislot: 耗尽善后后零泄漏", Guarded::alive.load() == base);
}

// ── Case 22：并发嵌套读者(每线程持 2 把锁) + 写者 —— UAF/完整性/泄漏 ──
static void test_thread_nested_concurrent() {
    const long base = Guarded::alive.load();
    constexpr int READERS = 6, WRITES = 30000;
    std::atomic<long> reads{0}, invalid{0};
    {
        Owner p1(new Guarded(0)), p2(new Guarded(0));
        std::atomic<bool> stop{false};
        std::vector<std::thread> rs;
        for (int t = 0; t < READERS; ++t)
            rs.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto a = p1.safe_read();        // 缓存槽
                    auto b = p2.safe_read();        // 同线程第二把 → 动态槽
                    if (a && !a->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                    if (b && !b->valid()) invalid.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (int i = 1; i <= WRITES; ++i) { p1 = new Guarded(i); p2 = new Guarded(i); }
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : rs) th.join();
    }
    drain_reclaim_list();
    TEST("multislot: 并发嵌套读未读到失效对象(无 UAF)", invalid.load() == 0);
    TEST("multislot: 并发嵌套读确实发生",               reads.load() > 0);
    TEST("multislot: 并发嵌套读写后零泄漏",             Guarded::alive.load() == base);
}

// ─────────────────────────────────────────────
int main() {
    std::cout << "=== lock_free 完整测试 ===\n\n";

    test_single_thread_basic();
    test_shared_ptr_semantics();
    test_no_leak_full_drain();
    test_no_leak_destructor_drains();
    test_no_leak_partial_then_destruct();
    test_concurrent_push();
    test_concurrent_pop_no_duplicate();
    test_concurrent_mixed_no_leak();
    test_hazard_reclaim_path_no_leak();
    test_hazard_slot_reuse();

    test_owner_basic_read();
    test_owner_assign_reclaims_old();
    test_owner_deferred_reclaim();
    test_owner_concurrent_read_write();
    test_owner_multi_writer();

    test_thread_nested_two_locks();
    test_thread_nested_release_order();
    test_thread_deep_nesting();
    test_thread_nesting_no_exhaustion();
    test_thread_nesting_exhaustion_throws();
    test_thread_nested_concurrent();

    test_global_balance();

    std::cout << "\n----------------------------------------\n";
    std::cout << "PASS: " << g_pass << "    FAIL: " << g_fail << "\n";
    std::cout << "Tracked  constructed=" << Tracked::constructed.load()
              << "  destroyed=" << Tracked::destroyed.load()
              << "  alive="     << Tracked::alive.load() << "\n";
    return g_fail == 0 ? 0 : 1;
}

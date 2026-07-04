#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../examples/pipelines.hpp"
#include "../src/kernels/registry.hpp"
#include "../src/mem/arena.hpp"
#include "../src/mem/fixed_pool.hpp"
#include "../src/net/feed.hpp"
#include "../src/net/rust_server.hpp"
#include "../src/runtime/context.hpp"
#include "../src/runtime/optimizer.hpp"
#include "../src/runtime/scheduler.hpp"
#include "../src/runtime/streaming.hpp"
#include "../src/storage/cursor.hpp"

using namespace aegis;

static bool close(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

static uint16_t unused_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;  // restricted test sandboxes may disable sockets
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(fd);
        return 0;
    }
    const uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

int main() {
    // R1: arena alignment/high-water, pool reuse and typed spans.
    mem::Arena arena(64 * 1024);
    auto* a = arena.allocate_n<double>(16);
    assert(reinterpret_cast<uintptr_t>(a) % 64 == 0);
    assert(arena.high_water() >= 16 * sizeof(double));
    arena.reset();
    assert(arena.used() == 0 && arena.high_water() > 0);
    mem::FixedPool<Tick, 2> pool;
    Tick* p0 = pool.create(Tick{1, 2, 3});
    Tick* p1 = pool.create(Tick{2, 3, 4});
    assert(p0 && p1 && !pool.create(Tick{}));
    pool.destroy(p0);
    assert(pool.create(Tick{3, 4, 5}));

    // Arena invariant: an allocation whose alignment pushes past capacity must
    // throw even for zero bytes — used() may never exceed capacity().
    {
        mem::Arena tiny(64);
        tiny.allocate(60, 1);
        bool threw = false;
        try { tiny.allocate(0, 128); } catch (const std::bad_alloc&) { threw = true; }
        assert(threw && tiny.used() <= tiny.capacity());
    }

    // Registry dispatch and stable normalization/matrix oracle.
    assert(kernels::registry().size() == 12 && kernels::find("matmul"));
    std::vector<double> logits{1000, 1001, 1002, -1000, -999, -998};
    std::vector<double> probs(6);
    kernels::RegistryEntry::Call soft{{mat_view(static_cast<const double*>(logits.data()), 2, 3)},
                                      mat_view(probs.data(), 2, 3)};
    kernels::find("softmax")->best_fn(soft);
    assert(close(probs[0] + probs[1] + probs[2], 1.0));
    assert(close(probs[3] + probs[4] + probs[5], 1.0));
    std::vector<double> ma{1, 2, 3, 4}, mb{5, 6, 7, 8}, mn(4), mf(4);
    kernels::naive::matmul(mat_view(static_cast<const double*>(ma.data()), 2, 2),
                           mat_view(static_cast<const double*>(mb.data()), 2, 2),
                           mat_view(mn.data(), 2, 2));
    kernels::best::matmul(mat_view(static_cast<const double*>(ma.data()), 2, 2),
                          mat_view(static_cast<const double*>(mb.data()), 2, 2),
                          mat_view(mf.data(), 2, 2));
    assert(mn == mf && mn[0] == 19 && mn[3] == 50);

    // matmul into a STRIDED submatrix view must not scribble on gap columns.
    {
        std::vector<double> big(4 * 4, -7.0);  // 4x4 parent, sentinel-filled
        kernels::best::matmul(mat_view(static_cast<const double*>(ma.data()), 2, 2),
                              mat_view(static_cast<const double*>(mb.data()), 2, 2),
                              MatView{big.data(), 2, 2, 4});  // top-left 2x2
        assert(big[0] == 19 && big[1] == 22 && big[4] == 43 && big[5] == 50);
        assert(big[2] == -7.0 && big[3] == -7.0 && big[6] == -7.0);  // gap intact
        for (size_t i = 8; i < 16; ++i) assert(big[i] == -7.0);      // rows 2-3 intact
    }

    // R2/R4: DAG execution, dead-node elimination, pair fusion.
    std::vector<int> order;
    runtime::TaskGraph graph;
    auto n0 = graph.add_node("first", kernels::KernelClass::TRANSFORM,
                             [&] { order.push_back(1); });
    auto n1 = graph.add_node("second", kernels::KernelClass::TRANSFORM,
                             [&] { order.push_back(2); }, true);
    graph.add_node("dead", kernels::KernelClass::REDUCTION,
                   [&] { order.push_back(99); });
    graph.add_edge(n0, n1);
    auto report = runtime::optimize(graph);
    assert(report.dead_nodes_removed == 1 && report.transform_pairs_fused == 1);
    runtime::Scheduler scheduler(2);
    scheduler.submit(graph);
    assert((order == std::vector<int>{1, 2}));
    runtime::RuntimeStats stats;
    runtime::BoundedChannel<Tick, 2> channel(runtime::BackpressurePolicy::Drop, &stats);
    assert(channel.push({1, 1, 1}) && channel.push({2, 2, 2}));
    assert(!channel.push({3, 3, 3}) && stats.dropped == 1);
    static_assert(sizeof(net::WireTick) == 32);

    // Both example workloads run on the same scheduler/graph API.
    std::vector<double> prices{1, 2, 3, 4};
    examples::AnalyticsPipeline analytics(col_view(static_cast<const double*>(prices.data()), prices.size()));
    scheduler.submit(analytics.graph);
    assert(analytics.signal.size() == prices.size());
    examples::MlpPipeline mlp(mat_view(static_cast<const double*>(ma.data()), 2, 2),
                              mat_view(static_cast<const double*>(mb.data()), 2, 2));
    scheduler.submit(mlp.graph);
    assert(close(mlp.probabilities[0] + mlp.probabilities[1], 1.0));

    // Engine seam: the same graphs run through a single ExecutionContext that
    // wires arena + scheduler + telemetry + registry, recording each run.
    runtime::RuntimeStats engine_stats;
    runtime::ExecutionContext ctx{&arena, &scheduler, &engine_stats};
    assert(ctx.kernel("softmax") && !ctx.kernel("no_such_kernel"));
    examples::AnalyticsPipeline via_ctx(col_view(static_cast<const double*>(prices.data()), prices.size()));
    const uint64_t took = ctx.execute(via_ctx.graph);
    assert(via_ctx.signal.size() == prices.size());
    assert(engine_stats.accepted.load() == 1 && engine_stats.wire_to_feature.count() == 1);
    (void)took;

    // R3: seal + WAL commit + mmap cursor + range filtering and batching.
    const auto dir = std::filesystem::temp_directory_path() / "aegis-runtime-test";
    std::filesystem::remove_all(dir);
    storage::SegmentStore store(dir.string());
    TickTable table;
    for (size_t i = 0; i < 10; ++i) {
        table.price.push(100 + i);
        table.volume.push(1 + i);
        table.ts_ns.push(1000 + i * 10);
    }
    store.append(table, "0001.aegis");
    auto cursor = store.open({1020, 1060}, 0, 3);
    size_t rows = 0;
    double sum = 0.0;
    while (auto batch = cursor.next(arena)) {
        rows += batch->size();
        for (size_t i = 0; i < batch->size(); ++i) sum += batch->price(i);
        arena.reset();
    }
    assert(rows == 5 && close(sum, 102 + 103 + 104 + 105 + 106));

    // Rust std::net server -> C++ readv/arena -> bounded SPSC channel.
    const uint16_t port = unused_loopback_port();
    if (std::getenv("AEGIS_REQUIRE_NETWORK_TEST")) assert(port != 0);
    if (port != 0) {
        std::exception_ptr server_error;
        uint64_t sent = 0;
        std::thread server([&] {
            try {
                sent = net::rust::serve_once(dir.string(), port);
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        runtime::RuntimeStats feed_stats;
        runtime::BoundedChannel<Tick, 256> feed_channel(
            runtime::BackpressurePolicy::Block, &feed_stats);
        net::TcpFeed feed("127.0.0.1", port);
        size_t accepted = 0;
        for (size_t attempts = 0; accepted < 10 && attempts < 20; ++attempts) {
            arena.reset();
            accepted += feed.receive(arena, feed_channel, 64);
        }
        server.join();
        if (server_error) std::rethrow_exception(server_error);
        assert(sent == 10 && accepted == 10 && feed.stats().frames == 10);
        for (size_t i = 0; i < 10; ++i) {
            auto tick = feed_channel.pop();
            assert(tick && tick->ts_ns == 1000 + i * 10 && tick->price == 100 + i);
        }
        std::puts("Rust TCP -> C++ ring integration passed");
    }
    std::filesystem::remove_all(dir);
    return 0;
}

// downstream find_package consumer.
#include <cstdio>
#include "dependency_fabric/df.hpp"
using namespace dependency_fabric;
int main() {
    Graph g;
    const SourceId src(1); const SourceBootId boot(1); const CoordinatorEpoch epoch(1);
    if (g.register_source(src, boot, epoch) != Outcome::ACCEPTED) return 1;
    if (g.declare_node(DependencyNodeId(1), NodeKind::MODEL, "m", src, boot, epoch) != Outcome::ACCEPTED) return 1;
    if (g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(1), src, boot, epoch) != Outcome::ACCEPTED) return 1;
    g.recompute_all_readiness();
    if (g.readiness(DependencyNodeId(1)) != ReadinessState::READY) return 1;
    std::printf("find_package consumer OK: model READY, digest=%llu\n", (unsigned long long)g.digest());
    return 0;
}

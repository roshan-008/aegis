#pragma once
// Deterministic replay — reproducibility as an executable check, not a claim.
//
// A run records a manifest: a fingerprint of the graph it executed (names,
// kernel classes, dependency edges, liveness) plus an FNV-1a checksum of
// every named output buffer. Replaying means re-executing the same graph
// over the same inputs and verifying the fresh manifest against the stored
// one. If the engine is deterministic, checksums match bit-for-bit; if
// anything drifted — kernel change, schedule-dependent reduction, uninit
// read — verify_replay() names the exact output that diverged.
//
// The storage layer already makes the *input* side reproducible (sealed
// CRC-checked segments, WAL recovery, paced ReplayCursor); this closes the
// output side. Manifests are a tiny flat JSON dialect written and parsed
// here with no dependencies; u64 values are hex strings because JSON
// numbers lose integer precision past 2^53.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "task_graph.hpp"

namespace aegis::runtime {

// FNV-1a 64-bit: tiny, dependency-free, stable across platforms for the
// little-endian byte images we feed it. Not cryptographic — a drift
// detector, not a tamper seal.
inline uint64_t fnv1a(const void* data, size_t bytes,
                      uint64_t hash = 0xcbf29ce484222325ULL) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

inline uint64_t checksum_doubles(const double* values, size_t n) {
    return fnv1a(values, n * sizeof(double));
}
inline uint64_t checksum_doubles(const std::vector<double>& values) {
    return checksum_doubles(values.data(), values.size());
}

// Structural fingerprint of a graph: node names, kernel classes, liveness,
// and the dependency edge list. Two runs can only be compared if they
// executed the same shape.
inline uint64_t graph_fingerprint(const TaskGraph& graph) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& node : graph.nodes()) {
        hash = fnv1a(node.name.data(), node.name.size(), hash);
        const unsigned char meta[2] = {static_cast<unsigned char>(node.klass),
                                       node.live ? (unsigned char)1 : (unsigned char)0};
        hash = fnv1a(meta, sizeof meta, hash);
        for (NodeId d : node.dependencies) {
            uint64_t edge = d;
            hash = fnv1a(&edge, sizeof edge, hash);
        }
    }
    return hash;
}

struct RunManifest {
    std::string run_id;
    uint64_t graph = 0;    // graph_fingerprint at execution time
    uint64_t workers = 0;  // scheduler width (recorded, not part of identity)
    uint64_t wall_ns = 0;  // informational; never compared
    std::vector<std::pair<std::string, uint64_t>> outputs;  // name -> checksum

    void add_output(const std::string& name, const std::vector<double>& v) {
        outputs.emplace_back(name, checksum_doubles(v));
    }
    void add_output(const std::string& name, const double* p, size_t n) {
        outputs.emplace_back(name, checksum_doubles(p, n));
    }
};

struct ReplayReport {
    bool graph_matches = false;
    bool outputs_match = false;  // same names, same order, same checksums
    std::vector<std::string> diverged;  // names whose checksums differ
    bool ok() const { return graph_matches && outputs_match; }
};

// Compare a recorded manifest with a freshly re-executed one. wall_ns and
// workers are deliberately excluded: timing may differ, results may not.
inline ReplayReport verify_replay(const RunManifest& recorded,
                                  const RunManifest& replayed) {
    ReplayReport report;
    report.graph_matches = recorded.graph == replayed.graph;
    report.outputs_match = recorded.outputs.size() == replayed.outputs.size();
    const size_t n = std::min(recorded.outputs.size(), replayed.outputs.size());
    for (size_t i = 0; i < n; ++i) {
        if (recorded.outputs[i].first != replayed.outputs[i].first) {
            report.outputs_match = false;
            report.diverged.push_back(recorded.outputs[i].first + "!=" +
                                      replayed.outputs[i].first);
        } else if (recorded.outputs[i].second != replayed.outputs[i].second) {
            report.outputs_match = false;
            report.diverged.push_back(recorded.outputs[i].first);
        }
    }
    return report;
}

// ---- manifest (de)serialization -------------------------------------------

inline std::string hex64(uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline void write_manifest(const RunManifest& m, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("manifest: cannot open " + path);
    std::fprintf(f,
                 "{\n  \"run_id\": \"%s\",\n  \"graph\": \"%s\",\n"
                 "  \"workers\": %llu,\n  \"wall_ns\": %llu,\n  \"outputs\": {",
                 m.run_id.c_str(), hex64(m.graph).c_str(),
                 static_cast<unsigned long long>(m.workers),
                 static_cast<unsigned long long>(m.wall_ns));
    for (size_t i = 0; i < m.outputs.size(); ++i)
        std::fprintf(f, "%s\n    \"%s\": \"%s\"", i ? "," : "",
                     m.outputs[i].first.c_str(),
                     hex64(m.outputs[i].second).c_str());
    const bool ok = std::fputs("\n  }\n}\n", f) >= 0;
    if (std::fclose(f) != 0 || !ok)
        throw std::runtime_error("manifest: short write to " + path);
}

// Minimal parser for the exact dialect written above. Strict on what it
// needs (keys, quoting, hex values), tolerant of whitespace.
inline RunManifest read_manifest(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("manifest: cannot open " + path);
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    std::fclose(f);

    auto quoted_after = [&](const std::string& key, size_t from) {
        const size_t k = text.find("\"" + key + "\"", from);
        if (k == std::string::npos)
            throw std::runtime_error("manifest: missing key " + key);
        const size_t open = text.find('"', text.find(':', k));
        const size_t close = text.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos)
            throw std::runtime_error("manifest: malformed value for " + key);
        return std::make_pair(text.substr(open + 1, close - open - 1), close);
    };
    auto number_after = [&](const std::string& key) -> uint64_t {
        const size_t k = text.find("\"" + key + "\"");
        if (k == std::string::npos)
            throw std::runtime_error("manifest: missing key " + key);
        return std::strtoull(text.c_str() + text.find(':', k) + 1, nullptr, 10);
    };

    RunManifest m;
    m.run_id = quoted_after("run_id", 0).first;
    m.graph = std::strtoull(quoted_after("graph", 0).first.c_str(), nullptr, 16);
    m.workers = number_after("workers");
    m.wall_ns = number_after("wall_ns");

    const size_t outputs_at = text.find("\"outputs\"");
    if (outputs_at == std::string::npos)
        throw std::runtime_error("manifest: missing outputs");
    size_t cursor = text.find('{', outputs_at);
    const size_t end = text.find('}', cursor);
    while (true) {
        const size_t open = text.find('"', cursor + 1);
        if (open == std::string::npos || open > end) break;
        const size_t close = text.find('"', open + 1);
        const std::string name = text.substr(open + 1, close - open - 1);
        const auto [value, at] = quoted_after(name, open);
        (void)at;
        m.outputs.emplace_back(name,
                               std::strtoull(value.c_str(), nullptr, 16));
        cursor = text.find(',', close);
        if (cursor == std::string::npos || cursor > end) break;
    }
    return m;
}

}  // namespace aegis::runtime

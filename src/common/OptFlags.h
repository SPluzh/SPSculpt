#pragma once

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <strings.h>
#endif

namespace OptFlags {

inline bool readEnvBool(const char* name, bool fallback) {
    const char* val = std::getenv(name);
    if (!val || !val[0]) return fallback;
#ifdef _WIN32
    if (_stricmp(val, "1") == 0 || _stricmp(val, "true") == 0 || _stricmp(val, "on") == 0 || _stricmp(val, "yes") == 0) {
        return true;
    }
    if (_stricmp(val, "0") == 0 || _stricmp(val, "false") == 0 || _stricmp(val, "off") == 0 || _stricmp(val, "no") == 0) {
        return false;
    }
#else
    if (strcasecmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "on") == 0 || strcasecmp(val, "yes") == 0) {
        return true;
    }
    if (strcasecmp(val, "0") == 0 || strcasecmp(val, "false") == 0 || strcasecmp(val, "off") == 0 || strcasecmp(val, "no") == 0) {
        return false;
    }
#endif
    return fallback;
}

struct Config {
    // Stage 1: Idle, Culling, VAO/Uniform cache, Adaptive Orbit Resolution
    bool idleSleep = false;
    bool cullFace = false;
    bool vaoCache = false;
    bool orbitScale = false;

    // Stage 2: Block GPU Upload & Indexed PolyGroup
    bool dirtyBlocks = false;
    bool indexedPolygroup = false;

    // Stage 3: CPU Quick Wins (No change to math)
    bool faceNormalsOnly = false;
    bool cpuReuse = false;
    bool moveOptim = false;

    // Stage 4: Coalescing, Batching, Cursor Throttle, BVH Leaf Refit
    bool coalesceGrab = false;
    bool batchSubsteps = false;
    bool cursorThrottle = false;
    bool bvhRefitLeaves = false;

    // Stage 5: Morton Clusters & Cluster Culling
    bool mortonClusters = false;

    static Config loadFromEnv() {
        Config c;
        const bool st1 = readEnvBool("SP_OPT_STAGE1", false);
        const bool st2 = readEnvBool("SP_OPT_STAGE2", false);
        const bool st3 = readEnvBool("SP_OPT_STAGE3", false);
        const bool st4 = readEnvBool("SP_OPT_STAGE4", false);
        const bool st5 = readEnvBool("SP_OPT_STAGE5", false);

        c.idleSleep = readEnvBool("SP_OPT_IDLE_SLEEP", st1);
        c.cullFace = readEnvBool("SP_OPT_CULL_FACE", st1);
        c.vaoCache = readEnvBool("SP_OPT_VAO_CACHE", st1);
        c.orbitScale = readEnvBool("SP_OPT_ORBIT_SCALE", st1);

        c.dirtyBlocks = readEnvBool("SP_OPT_DIRTY_BLOCKS", st2);
        c.indexedPolygroup = readEnvBool("SP_OPT_INDEXED_POLYGROUP", st2);

        c.faceNormalsOnly = readEnvBool("SP_OPT_FACE_NORMALS_ONLY", st3);
        c.cpuReuse = readEnvBool("SP_OPT_CPU_REUSE", st3);
        c.moveOptim = readEnvBool("SP_OPT_MOVE_OPTIM", st3);

        c.coalesceGrab = readEnvBool("SP_OPT_COALESCE_GRAB", st4);
        c.batchSubsteps = readEnvBool("SP_OPT_BATCH_SUBSTEPS", st4);
        c.cursorThrottle = readEnvBool("SP_OPT_CURSOR_THROTTLE", st4);
        c.bvhRefitLeaves = readEnvBool("SP_OPT_BVH_REFIT_LEAVES", st4);

        c.mortonClusters = readEnvBool("SP_OPT_MORTON_CLUSTERS", st5);
        return c;
    }
};

inline Config& get() {
    static Config instance = Config::loadFromEnv();
    return instance;
}

inline std::string formatSummary() {
    const Config& c = get();
    std::ostringstream oss;
    auto add = [&](const char* name, bool val) {
        if (val) {
            if (oss.tellp() > 0) oss << ", ";
            oss << name;
        }
    };
    add("IDLE_SLEEP", c.idleSleep);
    add("CULL_FACE", c.cullFace);
    add("VAO_CACHE", c.vaoCache);
    add("ORBIT_SCALE", c.orbitScale);
    add("DIRTY_BLOCKS", c.dirtyBlocks);
    add("INDEXED_PG", c.indexedPolygroup);
    add("FACE_NORM_ONLY", c.faceNormalsOnly);
    add("CPU_REUSE", c.cpuReuse);
    add("MOVE_OPTIM", c.moveOptim);
    add("COALESCE_GRAB", c.coalesceGrab);
    add("BATCH_SUBSTEPS", c.batchSubsteps);
    add("CURSOR_THROTTLE", c.cursorThrottle);
    add("BVH_REFIT_LEAVES", c.bvhRefitLeaves);
    add("MORTON_CLUSTERS", c.mortonClusters);
    std::string out = oss.str();
    return out.empty() ? std::string("NONE (Stage 0 Baseline)") : out;
}

} // namespace OptFlags


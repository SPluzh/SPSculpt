#ifndef COMMON_CRASH_REPORTER_H
#define COMMON_CRASH_REPORTER_H

#include <cstdint>
#include <string>

namespace CrashReporter {

struct PreviousCrashInfo {
    bool found = false;
    std::string reportPath;
    std::string dumpPath;
    std::string timestamp;
    std::string summary;
    std::string stage;
    std::string brushName;
};

struct DiagnosticPaths {
    std::string reportPath;
    std::string dumpPath;
};

// Initialize crash reporting hooks, logs/ directory, and startup metadata.
void init(bool headless = false);

// Re-apply OS handlers (helpful after graphics driver / SDL initialization).
void reinstallHandlers();

void setHeadless(bool headless);
bool isHeadless();

// Register readable thread name for crash reports (allocation-free lookup after first call).
void registerCurrentThread(const char* name);

// Very fast (~1ns) current execution stage marker. Pass string literals with static lifetime.
void setStage(const char* stage);
const char* getStage();

class ScopedStage {
public:
    explicit ScopedStage(const char* stage)
        : m_prev(getStage()) {
        setStage(stage);
    }
    ~ScopedStage() {
        setStage(m_prev);
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
private:
    const char* m_prev;
};

// Lock-free fixed ring-buffer breadcrumb event.
void recordBreadcrumb(const char* category, const char* fmt, ...);

// Fast lock-free stroke diagnostics snapshot (updated per brush pass).
void setStrokeActive(bool active);
void updateStrokeContext(int brushId,
                         const char* brushName,
                         uint64_t strokeSeq,
                         uint32_t passIndex,
                         uint32_t nbVerts,
                         uint32_t nbFaces,
                         uint32_t nbIVerts,
                         uint32_t nbIFaces,
                         bool dyntopo,
                         bool symmetry,
                         float radius,
                         float intensity,
                         float hitX,
                         float hitY,
                         float hitZ);
void updateStrokeCounts(uint32_t nbIVerts, uint32_t nbIFaces);
void clearStrokeContext();

// Write crash report .txt and .dmp immediately (for caught fatal errors or self-tests).
DiagnosticPaths writeDiagnosticSnapshot(const char* reason,
                                          const char* detail = nullptr,
                                          bool writeLastCrashFlag = false,
                                          bool showModalIfInteractive = false);

// Check and consume logs/last_crash.flag on startup. Returns true if a previous crash flag existed.
bool consumePreviousCrash(PreviousCrashInfo& outInfo);

// Get resolved logs/ directory path and open it in OS file explorer.
std::string getLogsDirectory();
bool openLogsDirectory();

} // namespace CrashReporter

#endif // COMMON_CRASH_REPORTER_H

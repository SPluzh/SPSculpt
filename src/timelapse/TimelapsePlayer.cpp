#include "timelapse/TimelapsePlayer.h"
#include "stb_image_write.h"
#include "common/Logger.h"
#include "mesh/Mesh.h"
#include <algorithm>
#include <filesystem>
#include <cstdio>

bool TimelapsePlayer::open(UndoManager& undo, Scene& scene) {
    if (m_isOpen) return true;

    auto stack = undo.takeUndoStack();
    if (stack.empty()) {
        sculpt_log("[TimelapsePlayer] Cannot open: undo history is empty.\n");
        return false;
    }

    m_timeline.clear();
    m_timeline.reserve(stack.size());
    while (!stack.empty()) {
        m_timeline.push_back(std::move(stack.front()));
        stack.pop_front();
    }

    m_totalSteps = static_cast<int>(m_timeline.size());
    m_savedStepOnOpen = m_totalSteps;
    m_steps.clear();
    m_steps.reserve(m_totalSteps);

    for (int i = 0; i < m_totalSteps; ++i) {
        TimelineStep step;
        step.stepIndex = i;
        if (m_timeline[i]) {
            step.type = m_timeline[i]->getType();
            step.description = m_timeline[i]->getDescription();
        } else {
            step.type = UndoEntryType::Sculpt;
            step.description = "Unknown Entry";
        }
        m_steps.push_back(step);
    }

    // Step backward to step 0 to capture the initial scene state
    for (int i = m_totalSteps - 1; i >= 0; --i) {
        UndoManager::applyEntry(m_timeline[i].get(), scene, true);
    }
    m_initialState = scene.saveCurrentState();

    // Replay forward to restore position at open (step N)
    for (int i = 0; i < m_totalSteps; ++i) {
        UndoManager::applyEntry(m_timeline[i].get(), scene, false);
    }

    m_currentStep = m_totalSteps;
    m_isOpen = true;
    m_state = State::PAUSED;
    m_accumTime = 0.0f;

    rebuildMeshState(scene);
    sculpt_log("[TimelapsePlayer] Player opened with %d steps in timeline.\n", m_totalSteps);
    return true;
}

void TimelapsePlayer::close(UndoManager& undo, Scene& scene) {
    if (!m_isOpen) return;

    // Restore scene state to the saved step on open (step N)
    seekToStep(m_savedStepOnOpen, scene);

    std::deque<std::unique_ptr<UndoEntry>> stack;
    for (auto& entry : m_timeline) {
        stack.push_back(std::move(entry));
    }
    m_timeline.clear();
    m_steps.clear();

    undo.restoreUndoStack(std::move(stack));

    m_isOpen = false;
    m_state = State::IDLE;
    sculpt_log("[TimelapsePlayer] Player closed, undo stack restored.\n");
}

void TimelapsePlayer::seekToStep(int targetStep, Scene& scene) {
    if (!m_isOpen || m_totalSteps == 0) return;
    targetStep = std::max(0, std::min(targetStep, m_totalSteps));

    if (targetStep == m_currentStep) return;

    if (targetStep < m_currentStep) {
        int diff = m_currentStep - targetStep;
        if (diff <= targetStep) {
            // Step backward incrementally
            for (int i = m_currentStep - 1; i >= targetStep; --i) {
                UndoManager::applyEntry(m_timeline[i].get(), scene, true);
            }
        } else {
            // Restore initial state and step forward
            scene.restoreState(m_initialState);
            for (int i = 0; i < targetStep; ++i) {
                UndoManager::applyEntry(m_timeline[i].get(), scene, false);
            }
        }
    } else {
        // Step forward incrementally
        for (int i = m_currentStep; i < targetStep; ++i) {
            UndoManager::applyEntry(m_timeline[i].get(), scene, false);
        }
    }

    m_currentStep = targetStep;
    rebuildMeshState(scene);
}

void TimelapsePlayer::stepForward(Scene& scene) {
    if (!m_isOpen) return;
    seekToStep(m_currentStep + 1, scene);
}

void TimelapsePlayer::stepBackward(Scene& scene) {
    if (!m_isOpen) return;
    seekToStep(m_currentStep - 1, scene);
}

void TimelapsePlayer::play(float speed) {
    if (!m_isOpen) return;
    setPlaySpeed(speed);
    m_state = State::PLAYING;
}

void TimelapsePlayer::pause() {
    if (!m_isOpen) return;
    m_state = State::PAUSED;
}

void TimelapsePlayer::togglePlayPause(Scene& scene) {
    if (!m_isOpen) return;
    if (m_state == State::PLAYING) {
        pause();
    } else {
        if (m_currentStep >= m_totalSteps) {
            seekToStep(0, scene);
        }
        play(m_playSpeed);
    }
}

void TimelapsePlayer::update(float deltaTime, Scene& scene) {
    if (!m_isOpen || m_state != State::PLAYING) return;

    m_accumTime += deltaTime;
    float stepInterval = 1.0f / m_playSpeed;

    while (m_accumTime >= stepInterval) {
        m_accumTime -= stepInterval;
        if (m_currentStep < m_totalSteps) {
            stepForward(scene);
        } else {
            m_state = State::PAUSED;
            m_accumTime = 0.0f;
            break;
        }
    }
}

bool TimelapsePlayer::exportFrames(Scene& scene, AngleRenderer& renderer,
                                  const std::string& outputDir,
                                  int width, int height,
                                  int stepsPerFrame,
                                  std::function<void(int current, int total)> progressCallback) {
    if (!m_isOpen || m_timeline.empty()) return false;
    State previousState = m_state;
    m_state = State::EXPORTING;

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);

    int savedStep = m_currentStep;
    seekToStep(0, scene);

    stepsPerFrame = std::max(1, stepsPerFrame);
    int totalFrames = (m_totalSteps + stepsPerFrame - 1) / stepsPerFrame;
    if (totalFrames == 0) totalFrames = 1;

    int frameIdx = 0;

    while (true) {
        for (Mesh* m : scene.getMeshes()) {
            if (m) {
                m->isDirty = true;
                renderer.uploadIfDirty(m);
            }
        }

        std::vector<uint8_t> pixels = renderer.renderToBuffer(scene, width, height);

        char filename[512];
        snprintf(filename, sizeof(filename), "%s/frame_%06d.png", outputDir.c_str(), frameIdx);
        stbi_write_png(filename, width, height, 4, pixels.data(), width * 4);

        if (progressCallback) {
            progressCallback(frameIdx + 1, totalFrames);
        }

        if (m_currentStep >= m_totalSteps) break;

        for (int s = 0; s < stepsPerFrame && m_currentStep < m_totalSteps; ++s) {
            stepForward(scene);
        }
        frameIdx++;
    }

    seekToStep(savedStep, scene);
    m_state = previousState == State::PLAYING ? State::PLAYING : State::PAUSED;
    return true;
}

std::string TimelapsePlayer::getCurrentDescription() const {
    if (m_currentStep == 0) {
        return "Initial State";
    }
    if (m_currentStep > 0 && m_currentStep <= static_cast<int>(m_steps.size())) {
        return m_steps[m_currentStep - 1].description;
    }
    return "";
}

void TimelapsePlayer::rebuildMeshState(Scene& scene) {
    for (Mesh* m : scene.getMeshes()) {
        if (!m) continue;
        m->isDirty = true;
    }
}

bool TimelapsePlayer::saveTimelapse(const std::string& filepath, const TimelapseMetadata& metadata) {
    if (!m_isOpen || m_timeline.empty()) {
        sculpt_log("[TimelapsePlayer] Cannot save timelapse: player is not open or timeline is empty.\n");
        return false;
    }

    return TimelapseSerializer::saveToFile(filepath, m_initialState, m_timeline, metadata);
}

bool TimelapsePlayer::loadTimelapse(const std::string& filepath, Scene& scene, TimelapseMetadata* outMetadata) {
    HistoryState loadedInitialState;
    std::vector<std::unique_ptr<UndoEntry>> loadedTimeline;
    TimelapseMetadata meta;

    if (!TimelapseSerializer::loadFromFile(filepath, loadedInitialState, loadedTimeline, meta)) {
        sculpt_log("[TimelapsePlayer] Failed to load timelapse from file: %s\n", filepath.c_str());
        return false;
    }

    if (outMetadata) {
        *outMetadata = meta;
    }

    m_timeline = std::move(loadedTimeline);
    m_initialState = std::move(loadedInitialState);
    m_totalSteps = static_cast<int>(m_timeline.size());
    m_savedStepOnOpen = m_totalSteps;

    m_steps.clear();
    m_steps.reserve(m_totalSteps);
    for (int i = 0; i < m_totalSteps; ++i) {
        TimelineStep step;
        step.stepIndex = i;
        if (m_timeline[i]) {
            step.type = m_timeline[i]->getType();
            step.description = m_timeline[i]->getDescription();
        } else {
            step.type = UndoEntryType::Sculpt;
            step.description = "Unknown Step";
        }
        m_steps.push_back(step);
    }

    scene.restoreState(m_initialState);
    m_currentStep = 0;
    m_isOpen = true;
    m_state = State::PAUSED;
    m_accumTime = 0.0f;

    rebuildMeshState(scene);
    sculpt_log("[TimelapsePlayer] Loaded .stlapse file with %d steps successfully.\n", m_totalSteps);
    return true;
}


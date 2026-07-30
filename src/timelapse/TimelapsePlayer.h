#pragma once

#include "editing/undo/UndoManager.h"
#include "editing/undo/UndoEntry.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"
#include <vector>
#include <memory>
#include <string>
#include <deque>
#include <functional>

class TimelapsePlayer {
public:
    enum class State { IDLE, PLAYING, PAUSED, EXPORTING };

    struct TimelineStep {
        int stepIndex = 0;
        UndoEntryType type = UndoEntryType::Sculpt;
        std::string description;
    };

    TimelapsePlayer() = default;
    ~TimelapsePlayer() = default;

    // Opens player: transfers stack from UndoManager, records timeline & initial state
    bool open(UndoManager& undo, Scene& scene);

    // Closes player: restores state & returns stack to UndoManager
    void close(UndoManager& undo, Scene& scene);

    bool isOpen() const { return m_isOpen; }

    // Navigation
    void seekToStep(int targetStep, Scene& scene);
    void stepForward(Scene& scene);
    void stepBackward(Scene& scene);
    void seekToStart(Scene& scene) { seekToStep(0, scene); }
    void seekToEnd(Scene& scene) { seekToStep(m_totalSteps, scene); }

    // Playback controls
    void play(float speed = 10.0f); // speed in steps/sec
    void pause();
    void togglePlayPause(Scene& scene);
    void update(float deltaTime, Scene& scene);

    // Export PNG frames sequence to disk
    bool exportFrames(Scene& scene, AngleRenderer& renderer,
                      const std::string& outputDir,
                      int width, int height,
                      int stepsPerFrame = 1,
                      std::function<void(int current, int total)> progressCallback = nullptr);

    // State getters / setters
    State getState() const { return m_state; }
    int getCurrentStep() const { return m_currentStep; }
    int getTotalSteps() const { return m_totalSteps; }
    float getProgress() const { return m_totalSteps > 0 ? (float)m_currentStep / (float)m_totalSteps : 0.0f; }
    float getPlaySpeed() const { return m_playSpeed; }
    void setPlaySpeed(float speed) { m_playSpeed = std::max(0.1f, std::min(200.0f, speed)); }
    const std::vector<TimelineStep>& getSteps() const { return m_steps; }
    std::string getCurrentDescription() const;

private:
    bool m_isOpen = false;
    State m_state = State::IDLE;
    int m_currentStep = 0;
    int m_totalSteps = 0;
    int m_savedStepOnOpen = 0;
    float m_playSpeed = 10.0f; // steps per second
    float m_accumTime = 0.0f;

    std::vector<std::unique_ptr<UndoEntry>> m_timeline;
    std::vector<TimelineStep> m_steps;
    HistoryState m_initialState;

    void rebuildMeshState(Scene& scene);
};

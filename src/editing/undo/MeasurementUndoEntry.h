#pragma once
#include "UndoEntry.h"
#include "editing/MeasurementTypes.h"
#include <vector>
#include <string>

class MeasurementUndoEntry : public UndoEntry {
public:
    std::string description = "Measurement change";
    std::vector<MeasurementSegment> beforeMeasure;
    std::vector<MeasurementSegment> afterMeasure;
    std::vector<MeasurementSegment> beforeDivider;
    std::vector<MeasurementSegment> afterDivider;

    UndoEntryType getType() const override { return UndoEntryType::Measurement; }

    size_t getMemoryUsage() const override {
        size_t total = sizeof(*this);
        total += (beforeMeasure.capacity() + afterMeasure.capacity() +
                  beforeDivider.capacity() + afterDivider.capacity()) * sizeof(MeasurementSegment);
        return total;
    }

    std::string getDescription() const override { return description; }
};

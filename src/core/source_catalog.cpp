#include "core/source_catalog.h"
#include <stdexcept>
namespace everything_lite {
void validateSource(const SourceRecord& s) {
    if (s.id < 0 || s.root.empty() || s.interval_seconds < 0 || s.revision < 0 ||
        static_cast<int>(s.type) < 0 || static_cast<int>(s.type) > static_cast<int>(SourceType::Unknown) ||
        static_cast<int>(s.mode) < 0 || static_cast<int>(s.mode) > static_cast<int>(IndexMode::Periodic) ||
        static_cast<int>(s.status) < 0 || static_cast<int>(s.status) > static_cast<int>(SourceStatus::Failed))
        throw std::invalid_argument("invalid source catalog entry");
}
}

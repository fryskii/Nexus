#pragma once
// Detailed process inspector window (one per process).
#include <cstdint>

namespace nx {

class App;

namespace ProcessWindow {
void Open(App& app, uint32_t pid);
void OnSnapshotAll();
void CloseAll();
} // namespace ProcessWindow

} // namespace nx

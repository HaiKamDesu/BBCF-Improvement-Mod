#pragma once

#include "D3D9EXWrapper/d3d9.h"

// Logs a one-time system-specs snapshot (OS build, CPU, RAM, GPU adapter +
// driver version, disk type/free space, and any running AV/security service
// matched against a known-name list) to DEBUG.txt at startup. Added so a bug
// report's DEBUG.txt is self-sufficient for hardware/software context without
// a separate round trip asking the reporter for their specs -- see the
// 2026-08-02 FPS-drop investigation (FrameStallDiagnostics).
void LogSystemSpecs(IDirect3DDevice9* device);

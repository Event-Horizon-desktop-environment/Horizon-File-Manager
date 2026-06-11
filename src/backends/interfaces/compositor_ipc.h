#pragma once
// Stub for standalone file browser

class NiriRuntime;
namespace wspace::hyprland { class HyprlandRuntime; }
namespace wspace::mango { class MangoRuntime; }
namespace wspace::sway { class SwayRuntime; }
namespace wspace::triad { class TriadRuntime; }

class CompositorRuntimeRegistry {
public:
  CompositorRuntimeRegistry() = default;
  ~CompositorRuntimeRegistry() = default;
  CompositorRuntimeRegistry(const CompositorRuntimeRegistry&) = delete;
  CompositorRuntimeRegistry& operator=(const CompositorRuntimeRegistry&) = delete;
};

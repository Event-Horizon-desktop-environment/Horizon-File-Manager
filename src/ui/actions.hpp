#pragma once

// hui::actions — named action registry shared by menus, buttons and
// shortcuts (the GtkApplication/GAction model, minus the toolkit).
//
// Today "can I paste here?" is answered separately in the context menu
// builder, the key handler and the toolbar, and the three drift. With
// actions there is exactly one predicate and one callback per command;
// every surface renders from the registry. Shortcuts are data on the
// action, so help overlays and conflict detection come free later.
//
// Dependency-free; see tests/test_actions.cpp.

#include <functional>
#include <string>
#include <vector>

namespace hui {

struct Action {
  std::string id;       // stable: "file.rename", "edit.paste" ...
  std::string label;    // human-readable, may contain '&' mnemonics later
  std::string shortcut; // display form, e.g. "Ctrl+V" (binding comes later)
  std::function<bool()> can_run; // null = always available
  std::function<void()> run;

  [[nodiscard]] bool enabled() const { return !can_run || can_run(); }
};

class ActionRegistry {
 public:
  void add(Action a) {
    for (auto& e : actions_) {
      if (e.id == a.id) {
        e = std::move(a);
        return;
      }
    }
    actions_.push_back(std::move(a));
  }

  [[nodiscard]] const Action* get(const std::string& id) const {
    for (auto& a : actions_)
      if (a.id == id) return &a;
    return nullptr;
  }

  // Runs the action when known and enabled. Returns true when it ran.
  bool run(const std::string& id) const {
    const Action* a = get(id);
    if (!a || !a->run || !a->enabled()) return false;
    a->run();
    return true;
  }

  [[nodiscard]] size_t size() const { return actions_.size(); }

 private:
  std::vector<Action> actions_;
};

} // namespace hui

// test_actions.cpp — unit tests for ui/actions.hpp.

#include "ui/actions.hpp"

#include <cstdio>

using namespace hui;

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                          \
  } while (0)

int main() {
  ActionRegistry reg;
  int runs = 0;
  bool gate = false;

  reg.add(Action{"file.rename", "Rename", "F2", nullptr, [&] { ++runs; }});
  reg.add(Action{"edit.paste", "Paste", "Ctrl+V", [&] { return gate; },
                 [&] { ++runs; }});

  CHECK(reg.size() == 2);
  CHECK(reg.run("file.rename") == true);
  CHECK(runs == 1);
  // Disabled action does not run.
  CHECK(reg.run("edit.paste") == false);
  CHECK(runs == 1);
  gate = true;
  CHECK(reg.run("edit.paste") == true);
  CHECK(runs == 2);
  // Unknown id does not run.
  CHECK(reg.run("nope.nope") == false);
  // Re-adding replaces.
  reg.add(Action{"file.rename", "Rename2", "F2", nullptr, [&] { runs += 10; }});
  CHECK(reg.size() == 2);
  CHECK(reg.get("file.rename")->label == "Rename2");
  CHECK(reg.run("file.rename") == true);
  CHECK(runs == 12);
  // Action without callback never runs.
  reg.add(Action{"noop.noop", "Noop", "", nullptr, nullptr});
  CHECK(reg.run("noop.noop") == false);

  if (failures == 0) std::printf("actions: all tests passed\n");
  return failures == 0 ? 0 : 1;
}

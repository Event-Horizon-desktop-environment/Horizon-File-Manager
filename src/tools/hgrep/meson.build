# horizon-grep: parallel literal search (C++ stdlib only).
# NOTE: an io_uring batch path was tried and reverted — barrier-heavy
# staging measured 2x SLOWER than the sync path (47ms vs 22ms on
# /usr/include). The sync path stays: open/read/close, no fstat.
executable('horizon-grep',
  files('main.cpp', 'search.cpp', 'ignore.cpp'),
  install: true,
)

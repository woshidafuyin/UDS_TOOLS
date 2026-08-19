#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace uds {

class ScopedHighResolutionTimer final {
public:
  ScopedHighResolutionTimer() noexcept {
#ifdef _WIN32
    active_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
  }

  ~ScopedHighResolutionTimer() {
#ifdef _WIN32
    if (active_) timeEndPeriod(1);
#endif
  }

  ScopedHighResolutionTimer(const ScopedHighResolutionTimer&) = delete;
  ScopedHighResolutionTimer& operator=(const ScopedHighResolutionTimer&) =
      delete;

private:
  bool active_{};
};

} // namespace uds

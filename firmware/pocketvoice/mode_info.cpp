// Status and Log: the device explaining itself without a laptop.

#include "pv.h"

void statusIdle() {
  faceClear();
  caption("STATUS");
  drawStatusBody();
  hint("B: next mode", kMuted);
}

void logIdle() {
  faceClear();
  caption("LOG");
  drawLogBody();
  hint("B: next mode", kMuted);
}

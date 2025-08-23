
#include "arcfour.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace std;

using uint8 = uint8_t;
static_assert (sizeof(uint8) == 1, "Char must be one byte.");

static void Initialize(const uint8 (&kk)[256],
		       uint8 (&ss)[256]) {
  uint8 i = 0, j = 0;
  for (int n = 256; n--;) {
    j += ss[i] + kk[i];
    uint8 t = ss[i];
    ss[i] = ss[j];
    ss[j] = t;
    i++;
  }
}

ArcFour::ArcFour(const vector<uint8> &v) : ii(0), jj(0) {
  uint8 kk[256];
  for (int i = 0; i < 256; i++) {
    ss[i] = i;
    kk[i] = v[i % v.size()];
  }
  Initialize(kk, ss);
}

ArcFour::ArcFour(const string &s) : ii(0), jj(0) {
  uint8 kk[256];
  for (int i = 0; i < 256; i++) {
    ss[i] = i;
    kk[i] = (uint8)s[i % s.size()];
  }
  Initialize(kk, ss);
}

uint8 ArcFour::Byte() {
  ii++;
  jj += ss[ii];
  uint8 ti = ss[ii];
  uint8 tj = ss[jj];
  ss[ii] = tj;
  ss[jj] = ti;

  return ss[(ti + tj) & 255];
}

void ArcFour::Discard(int n) {
  while (n--) (void)Byte();
}

void ArcFour::GetState(vector<uint8> *state) const {
  state->clear();
  state->reserve(2 + 256);
  state->push_back(ii);
  state->push_back(jj);
  for (int i = 0; i < 256; i++) state->push_back(ss[i]);
}

bool ArcFour::SetState(const vector<uint8> &state) {
  if (state.size() != 2 + 256) return false;
  ii = state[0];
  jj = state[1];
  for (int i = 0; i < 256; i++) ss[i] = state[2 + i];
  return true;
}



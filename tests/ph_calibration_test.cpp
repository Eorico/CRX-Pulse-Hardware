#include <cassert>
#include <cmath>

// Arduino supplies constrain() in the sketch; this shim lets the host test
// exercise the real interpolation without including the credential block.
inline float constrain(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

#include "ph_calibration.h"

int main() {
  assert(std::fabs(PhCalibration::valueOf(3.715f) - 6.86f) < 1e-5f);
  assert(std::fabs(PhCalibration::valueOf(3.970f) - 4.01f) < 1e-5f);
  assert(std::fabs(PhCalibration::valueOf(2.680f) - 9.18f) < 1e-5f);
  assert(PhCalibration::V_PH4 > PhCalibration::V_PH7);
  assert(PhCalibration::V_PH7 > PhCalibration::V_PH9);
  assert(PhCalibration::valueOf(6.000f) == 0.0f);
  assert(PhCalibration::valueOf(0.000f) == 14.0f);
  return 0;
}

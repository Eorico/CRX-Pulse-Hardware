#include <cmath>

// Arduino supplies constrain() in the sketch; this shim lets the host test
// exercise the real interpolation without including the credential block.
inline float constrain(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

#include "ph_calibration.h"

#define CHECK(condition) \
  do {                   \
    if (!(condition)) {  \
      return 1;          \
    }                    \
  } while (0)

int main() {
  CHECK(std::fabs(PhCalibration::valueOf(3.715f) - 6.86f) < 1e-5f);
  CHECK(std::fabs(PhCalibration::valueOf(3.970f) - 4.01f) < 1e-5f);
  CHECK(std::fabs(PhCalibration::valueOf(2.680f) - 9.18f) < 1e-5f);
  CHECK(PhCalibration::V_PH4 > PhCalibration::V_PH7);
  CHECK(PhCalibration::V_PH7 > PhCalibration::V_PH9);
  CHECK(PhCalibration::valueOf(6.000f) == 0.0f);
  CHECK(PhCalibration::valueOf(0.000f) == 14.0f);
  return 0;
}

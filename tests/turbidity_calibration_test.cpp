#include <cassert>
#include <cmath>
#include <cstring>

#include "turbidity_calibration.h"

static void expectClose(float actual, float expected) {
  assert(std::fabs(actual - expected) < 0.01f);
}

int main() {
  const float voltages[] = {0.00000f, 1.19160f, 1.20000f, 1.22865f,
                            1.25000f, 1.28065f, 1.30000f, 1.33015f,
                            1.35000f, 1.36460f, 1.39600f, 1.40400f,
                            3.30000f};
  const float percents[] = {100.0f, 100.0f, 95.148178f, 78.6f,
                            66.241635f, 48.5f, 37.32f, 19.9f,
                            8.433672f, 0.0f, 0.0f, 0.0f,
                            0.0f};
  const int count = sizeof(voltages) / sizeof(voltages[0]);

  for (int i = 0; i < count; i++) {
    expectClose(TurbidityCalibration::percentOf(voltages[i]), percents[i]);
  }

  assert(std::strcmp(TurbidityCalibration::statusOf(0.0f), "Clear") == 0);
  assert(std::strcmp(TurbidityCalibration::statusOf(19.9f), "Clear") == 0);
  assert(std::strcmp(TurbidityCalibration::statusOf(48.5f),
                     "Slightly Turbid") == 0);
  assert(std::strcmp(TurbidityCalibration::statusOf(78.6f), "Turbid") == 0);
  assert(std::strcmp(TurbidityCalibration::statusOf(78.61f),
                     "Highly Turbid") == 0);

  assert(TurbidityCalibration::percentOf(1.396f) == 0.0f);
  assert(TurbidityCalibration::percentOf(1.404f) == 0.0f);

  return 0;
}

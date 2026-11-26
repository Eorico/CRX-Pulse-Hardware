#ifndef TURBIDITY_CALIBRATION_H
#define TURBIDITY_CALIBRATION_H

// SEN0189 relative turbidity, from the standard five-point table.
// Voltage falls as turbidity rises; the device reports the percentage
// and this same class decides its status band.
class TurbidityCalibration {
 public:
  // Volts descending, percentages ascending. The outer two points are
  // the calibrated ends; the middle three are the standard's band edges.
  static const int POINT_COUNT = 5;
  static const float V_POINTS[POINT_COUNT];    // volts, DESCENDING
  static const float PCT_POINTS[POINT_COUNT];  // matching percentages

  static constexpr float V_CLEAR = 1.36460;  //   0.00 % relative turbidity
  static constexpr float V_HIGH  = 1.19160;  // 100.00 % relative turbidity

  // Each cut is the TOP of its band, so equality stays in the band the
  // standard names: 19.9 is Clear, 48.5 is Slightly, 78.6 is Turbid.
  static constexpr float CLEAR_MAX    = 19.9;
  static constexpr float SLIGHTLY_MAX = 48.5;
  static constexpr float TURBID_MAX   = 78.6;

  // Deliberately just inside each rail: a powered sensor never reaches
  // 0.016 V or 3.284 V.
  static const int ADC_MIN_VALID = 20;    // 0.016 V -- held at ground
  static const int ADC_MAX_VALID = 4075;  // 3.284 V -- railed high

  /// Relative turbidity % for a pin voltage, by interpolation along
  /// the table above. Flat at 0 / 100 outside the measured ends.
  static float percentOf(float volts) {
    if (volts >= V_POINTS[0])               return 0.0;
    if (volts <= V_POINTS[POINT_COUNT - 1]) return 100.0;

    for (int i = 0; i < POINT_COUNT - 1; i++) {
      const float vHi = V_POINTS[i];
      const float vLo = V_POINTS[i + 1];
      if (volts <= vHi && volts >= vLo) {
        return PCT_POINTS[i] +
               (vHi - volts) * (PCT_POINTS[i + 1] - PCT_POINTS[i]) / (vHi - vLo);
      }
    }
    return 100.0;
  }

  /// One ladder, used by the serial print and the LCD. Takes the
  /// PERCENTAGE, so it cannot drift from the published number.
  static const char* statusOf(float percent) {
    if (percent <= CLEAR_MAX)    return "Clear";
    if (percent <= SLIGHTLY_MAX) return "Slightly Turbid";
    if (percent <= TURBID_MAX)   return "Turbid";
    return "Highly Turbid";
  }
};

const float TurbidityCalibration::V_POINTS[TurbidityCalibration::POINT_COUNT] =
    { 1.36460, 1.33015, 1.28065, 1.22865, 1.19160 };
const float TurbidityCalibration::PCT_POINTS[TurbidityCalibration::POINT_COUNT] =
    { 0.0, 19.9, 48.5, 78.6, 100.0 };

#endif

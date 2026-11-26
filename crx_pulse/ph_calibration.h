#ifndef PH_CALIBRATION_H
#define PH_CALIBRATION_H

// =====================================================
// pH CALIBRATION  (3-point, explicit buffer standards)
// =====================================================
//
// The buffers are named by their standard values, not by a formula
// that assumes 7.00 / 4.00:
//
//   buffer    true pH   probe volts
//   neutral      6.86      3.715
//   acid         4.01      3.970
//   base         9.18      2.680
//
// Voltages are reconstructed probe output (pin voltage x 2 divider).
// The app records the same three constants so its reference test can
// catch drift between the two.
class PhCalibration {
 public:
  static constexpr float PH_NEUTRAL = 6.86;
  static constexpr float PH_ACID    = 4.01;
  static constexpr float PH_BASE    = 9.18;

  static constexpr float V_PH7 = 3.715;  // voltage_pH7, 6.86 buffer
  static constexpr float V_PH4 = 3.970;  // voltage_pH4, 4.01 buffer
  static constexpr float V_PH9 = 2.680;  // voltage_pH9, 9.18 buffer

  static constexpr float DIVIDER_RATIO = 2.0;

  /// Two-segment interpolation about the neutral point.
  ///
  /// The electrode's sensitivity is not symmetric across the buffers, so
  /// a single straight fit through all three would miss the outer ones.
  static float valueOf(float probeVolts) {
    float pH;
    if (probeVolts >= V_PH7) {
      pH = PH_NEUTRAL + (probeVolts - V_PH7) * (PH_ACID - PH_NEUTRAL) /
                        (V_PH4 - V_PH7);
    } else {
      pH = PH_NEUTRAL + (probeVolts - V_PH7) * (PH_BASE - PH_NEUTRAL) /
                        (V_PH9 - V_PH7);
    }
    return constrain(pH, 0.0, 14.0);
  }
};

#endif  // PH_CALIBRATION_H

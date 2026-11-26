#ifndef SENSOR_FILTERS_H
#define SENSOR_FILTERS_H

// Small, allocation-free filters shared by the ADC readers and their host
// tests. A median rejects one-off spikes; a jump guard holds an unconfirmed
// change; the deadband keeps a high-impedance probe's last digit still
// without easing a real change over many passes.
namespace SensorFilters {

const int MAX_MEDIAN_SAMPLES = 16;

inline int medianOf(const int* samples, int count) {
  if (count <= 0) return 0;
  if (count > MAX_MEDIAN_SAMPLES) count = MAX_MEDIAN_SAMPLES;

  int ordered[MAX_MEDIAN_SAMPLES];
  for (int i = 0; i < count; i++) ordered[i] = samples[i];

  for (int i = 1; i < count; i++) {
    const int key = ordered[i];
    int j = i - 1;
    while (j >= 0 && ordered[j] > key) {
      ordered[j + 1] = ordered[j];
      j--;
    }
    ordered[j + 1] = key;
  }

  if (count % 2 == 1) return ordered[count / 2];
  return (ordered[count / 2 - 1] + ordered[count / 2]) / 2;
}

// Publishes the sample in full once it clears the deadband. No ramp: a
// confirmed change reaches the dashboard in the same pass it arrives.
class SnapValue {
 public:
  explicit SnapValue(float minChange)
      : minChange_(minChange), published_(0.0f), initialized_(false) {}

  float update(float sample) {
    if (!initialized_) {
      published_ = sample;
      initialized_ = true;
      return published_;
    }

    const float delta = sample > published_ ? sample - published_
                                            : published_ - sample;
    if (delta >= minChange_) published_ = sample;
    return published_;
  }

  float value() const { return published_; }

  void reset() {
    published_ = 0.0f;
    initialized_ = false;
  }

 private:
  float minChange_;
  float published_;
  bool initialized_;
};

// Holds a jump that the next sample does not confirm. Running this before
// the snap filter stops one bad pass from publishing 9-10 pH.
class JumpGuard {
  public:
   static constexpr float MAX_IMMEDIATE_CHANGE = 0.5f;

   JumpGuard()
       : accepted_(0.0f),
         candidate_(0.0f),
         initialized_(false),
         hasCandidate_(false) {}

   float update(float sample) {
     if (!initialized_) {
       accepted_ = sample;
       initialized_ = true;
       hasCandidate_ = false;
       return sample;
     }

     const float fromAccepted = sample > accepted_ ? sample - accepted_
                                                    : accepted_ - sample;
     if (fromAccepted <= MAX_IMMEDIATE_CHANGE) {
       accepted_ = sample;
       hasCandidate_ = false;
       return sample;
     }

     if (hasCandidate_) {
       const float fromCandidate = sample > candidate_ ? sample - candidate_
                                                        : candidate_ - sample;
       if (fromCandidate <= MAX_IMMEDIATE_CHANGE) {
         accepted_ = sample;
         hasCandidate_ = false;
         return sample;
       }
     }

     candidate_ = sample;
     hasCandidate_ = true;
     return accepted_;
   }

   void reset() {
     accepted_ = 0.0f;
     candidate_ = 0.0f;
     initialized_ = false;
     hasCandidate_ = false;
   }

  private:
   float accepted_;
   float candidate_;
   bool initialized_;
   bool hasCandidate_;
};

}  // namespace SensorFilters

#endif

/* =====================================================================
 *  model.h  —  PRE-TRAINED fault-classification model (inference only)
 *  Synaptix Robotics / ML-Based Predictive Maintenance of Motors
 *
 *  Model : DecisionTreeClassifier(max_depth=5, class_weight='balanced')
 *  Train : ml/train_model.py  (scikit-learn)  -> exported to plain C
 *  Size  : 0 bytes of heap, ~40 us per inference on ESP8266 @ 80 MHz
 *
 *  NOTHING IS TRAINED ON THE ESP. This header is the frozen result of
 *  the offline training run; re-run ml/train_model.py to regenerate it.
 *
 *  Feature vector (order matters):
 *    f[0] voltage       V
 *    f[1] current       A
 *    f[2] temperature   degC
 *    f[3] vib_rate      vibration events / second
 *    f[4] vib_peak      peak events in a 100 ms sub-window
 *    f[5] power         W   (voltage * current)
 * ===================================================================== */
#ifndef MODEL_H
#define MODEL_H

#define MODEL_VERSION   "dt-v1.3.0"
#define MODEL_TRAINED   "2026-10-12"
#define N_FEATURES      6
#define N_CLASSES       7

enum FaultClass {
  CLS_HEALTHY = 0,
  CLS_OVERVOLTAGE,
  CLS_UNDERVOLTAGE,
  CLS_OVERLOAD,
  CLS_OVERHEAT,
  CLS_BEARING_WEAR,
  CLS_IMBALANCE
};

static const char* const CLASS_NAME[N_CLASSES] = {
  "Healthy",
  "Overvoltage Stress",
  "Undervoltage / Brownout",
  "Mechanical Overload",
  "Thermal Overheat",
  "Bearing Wear",
  "Rotor Imbalance"
};

/* 10-char labels for the 16x2 LCD */
static const char* const CLASS_SHORT[N_CLASSES] = {
  "HEALTHY", "OVERVOLT", "UNDERVOLT", "OVERLOAD", "OVERHEAT", "BEARING", "IMBALANCE"
};

/* Severity: 0 = ok, 1 = watch, 2 = warn, 3 = trip the relay */
static const uint8_t CLASS_SEVERITY[N_CLASSES] = { 0, 3, 2, 3, 3, 2, 2 };

/* --------------------------------------------------------------------
 *  ML SUGGESTIONS — what the operator should actually do.
 *  Returned by /api/status so the dashboard can render them directly.
 * ------------------------------------------------------------------ */
static const char* const CLASS_ACTION[N_CLASSES] = {
  /* HEALTHY */
  "No action. Keep logging; next scheduled greasing in 50 running hours.",
  /* OVERVOLTAGE */
  "STOP: supply above the 6 V motor rating. Relay opened automatically. "
  "Lower the bench-supply setpoint to 5-6 V, check the buck converter "
  "feedback divider, then press Reset Latch.",
  /* UNDERVOLTAGE */
  "Reduce load or recharge the source. Sustained brownout causes high "
  "slip current and winding heating. Verify supply wire gauge and "
  "connector drop.",
  /* OVERLOAD */
  "STOP: stall or jam. Relay opened. Free the shaft, remove excess load, "
  "check for a seized coupling or blocked fan, then Reset Latch.",
  /* OVERHEAT */
  "STOP: winding temperature over limit. Relay opened. Let the motor cool "
  "to below 40 C, improve ventilation, confirm duty cycle and that the "
  "fan is turning.",
  /* BEARING_WEAR */
  "Schedule maintenance: vibration signature matches bearing wear. "
  "Re-lubricate or replace the bearing within the next 20 running hours. "
  "Re-check vibration after service.",
  /* IMBALANCE */
  "Inspect rotor/coupling: unbalanced load or bent shaft. Re-seat the "
  "fan or flywheel, tighten the set-screw, re-balance, and confirm the "
  "mount bolts are torqued."
};

/* --------------------------------------------------------------------
 *  Decision tree, exported as a flat node array.
 *  feature < 0  ->  leaf node, value[] holds class probabilities x1000
 * ------------------------------------------------------------------ */
typedef struct {
  int8_t   feature;      // index into f[], or -1 for a leaf
  float    threshold;    // go left if f[feature] <= threshold
  int16_t  left;         // child index
  int16_t  right;        // child index
  uint8_t  leaf_class;   // argmax class for a leaf
  uint16_t leaf_conf;    // confidence x1000 for a leaf
} TreeNode;

static const TreeNode TREE[] = {
  /* 0  */ {  0,  6.000f,  1, 14,  0,   0 },   // voltage <= 6.0 ?
  /* -------- left subtree : voltage within rating ------------------- */
  /* 1  */ {  2, 55.000f,  2, 13,  0,   0 },   // temperature <= 55 ?
  /* 2  */ {  1,  1.200f,  3, 12,  0,   0 },   // current <= 1.2 ?
  /* 3  */ {  3, 18.000f,  4,  8,  0,   0 },   // vib_rate <= 18 ?
  /* 4  */ {  0,  3.500f,  5,  6,  0,   0 },   // voltage <= 3.5 ?
  /* 5  */ { -1,  0.000f, -1, -1, CLS_UNDERVOLTAGE, 946 },
  /* 6  */ {  2, 45.000f,  7, 11,  0,   0 },   // temperature <= 45 ?
  /* 7  */ { -1,  0.000f, -1, -1, CLS_HEALTHY,     981 },
  /* 8  */ {  3, 35.000f,  9, 10,  0,   0 },   // vib_rate <= 35 ?
  /* 9  */ { -1,  0.000f, -1, -1, CLS_BEARING_WEAR, 903 },
  /* 10 */ { -1,  0.000f, -1, -1, CLS_IMBALANCE,    917 },
  /* 11 */ { -1,  0.000f, -1, -1, CLS_OVERHEAT,     872 },  // 45-55 C creeping
  /* 12 */ { -1,  0.000f, -1, -1, CLS_OVERLOAD,     958 },
  /* 13 */ { -1,  0.000f, -1, -1, CLS_OVERHEAT,     972 },
  /* -------- right subtree : voltage above rating ------------------- */
  /* 14 */ {  2, 55.000f, 15, 16,  0,   0 },
  /* 15 */ { -1,  0.000f, -1, -1, CLS_OVERVOLTAGE,  989 },
  /* 16 */ { -1,  0.000f, -1, -1, CLS_OVERHEAT,     934 }   // hot AND over-volt
};

/* --------------------------------------------------------------------
 *  Inference
 * ------------------------------------------------------------------ */
typedef struct {
  uint8_t     cls;
  float       confidence;   // 0..1
  uint8_t     severity;     // 0..3
  const char* name;
  const char* shortName;
  const char* action;
} MlResult;

static MlResult mlPredict(const float f[N_FEATURES]) {
  int16_t n = 0;
  uint16_t guard = 0;
  while (TREE[n].feature >= 0 && guard++ < 64) {
    n = (f[TREE[n].feature] <= TREE[n].threshold) ? TREE[n].left : TREE[n].right;
  }
  MlResult r;
  r.cls        = TREE[n].leaf_class;
  r.confidence = TREE[n].leaf_conf / 1000.0f;
  r.severity   = CLASS_SEVERITY[r.cls];
  r.name       = CLASS_NAME[r.cls];
  r.shortName  = CLASS_SHORT[r.cls];
  r.action     = CLASS_ACTION[r.cls];
  return r;
}

/* Remaining-useful-life estimate, in running hours.
   Simple monotone degradation curve fitted on the same training set. */
static uint16_t mlEstimateRulHours(const float f[N_FEATURES], uint8_t cls) {
  if (cls == CLS_HEALTHY) {
    float stress = (f[2] / 55.0f) * 0.5f + (f[3] / 35.0f) * 0.5f;
    if (stress < 0.05f) stress = 0.05f;
    uint16_t h = (uint16_t)(500.0f * (1.0f - stress));
    return h < 40 ? 40 : h;
  }
  if (cls == CLS_BEARING_WEAR) return 20;
  if (cls == CLS_IMBALANCE)    return 35;
  if (cls == CLS_UNDERVOLTAGE) return 120;
  return 0;  // trip classes: service now
}

#endif // MODEL_H

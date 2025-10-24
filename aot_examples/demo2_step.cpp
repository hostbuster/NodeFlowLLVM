#include "demo2_step.h"
#ifdef __cplusplus
extern "C" {
#endif
const int NODEFLOW_NUM_TOPO = 6;
const int NODEFLOW_TOPO_ORDER[6] = {0,1,2,3,4,5};
const int NODEFLOW_NUM_PORTS = 11;
const NodeFlowPortDesc NODEFLOW_PORTS[11] = {
  { 0, "key1", "out1", 1, "int" },
  { 1, "key2", "out1", 1, "int" },
  { 2, "random1", "out1", 1, "float" },
  { 3, "metronome1", "out1", 1, "double" },
  { 4, "counter1", "in1", 0, "int" },
  { 5, "counter1", "out1", 1, "int" },
  { 6, "add1", "in1", 0, "float" },
  { 7, "add1", "in2", 0, "float" },
  { 8, "add1", "in3", 0, "float" },
  { 9, "add1", "in4", 0, "float" },
  { 10, "add1", "out1", 1, "float" }
};

const int NODEFLOW_NUM_INPUT_FIELDS = 3;
const NodeFlowInputField NODEFLOW_INPUT_FIELDS[3] = {
  { "key1", offsetof(NodeFlowInputs, key1), "int" },
  { "key2", offsetof(NodeFlowInputs, key2), "int" },
  { "random1", offsetof(NodeFlowInputs, random1), "float" }
};

void nodeflow_init(NodeFlowState* s) {
  s->acc_metronome1 = 0.0; s->tout_metronome1 = 0.0f;
  s->last_counter1 = 0; s->cnt_counter1 = 0.0;
}
void nodeflow_reset(NodeFlowState* s) { nodeflow_init(s); }
void nodeflow_set_input(int handle, double value, NodeFlowInputs* in, NodeFlowState*) {
  if (handle == 0) in->key1 = (int)value;
  if (handle == 1) in->key2 = (int)value;
  if (handle == 2) in->random1 = (float)value;
}
double nodeflow_get_output(int handle, const NodeFlowOutputs* out, const NodeFlowState* s) {
  if (handle == 3) return (double)s->tout_metronome1;
  if (handle == 5) return s->cnt_counter1;
  if (handle == 10) return (double)out->add1;
  (void)out; (void)s; return 0.0;
}

void nodeflow_tick(double dt_ms, const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState* s) {
  s->tout_metronome1 = (double)0;
  s->acc_metronome1 += dt_ms; if (s->acc_metronome1 >= 3000) { s->acc_metronome1 -= 3000; s->tout_metronome1 = (double)1; }
  { int tick = (s->tout_metronome1 > 0.5f) ? 1 : 0; if (tick==1 && s->last_counter1==0) s->cnt_counter1+=1.0; s->last_counter1 = tick; }
  (void)in; (void)out; }

void nodeflow_step(const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState* s) {
  int _key1 = 0;
  int _key2 = 0;
  float _random1 = 0;
  double _metronome1 = 0;
  int _counter1 = 0;
  float _add1 = 0;

  _key1 = in->key1;
  _key2 = in->key2;
  _random1 = in->random1;
  _metronome1 = s->tout_metronome1;
  _counter1 = (float)s->cnt_counter1;
  _add1 = (float)_key1 + (float)_key2 + (float)_random1 + (float)_counter1;

  out->add1 = _add1;
}
#ifdef __cplusplus
}
#endif

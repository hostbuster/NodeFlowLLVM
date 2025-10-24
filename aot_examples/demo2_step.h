#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct {
  int key1;
  int key2;
  float random1;
} NodeFlowInputs;
typedef struct {
  float add1;
} NodeFlowOutputs;
typedef struct {
  double acc_metronome1;
  double tout_metronome1;
  int last_counter1;
  double cnt_counter1;
} NodeFlowState;
void nodeflow_step(const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState* state);
void nodeflow_tick(double dt_ms, const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState* state);
typedef struct { int handle; const char* nodeId; const char* portId; int is_output; const char* dtype; } NodeFlowPortDesc;
extern const int NODEFLOW_NUM_PORTS;
extern const NodeFlowPortDesc NODEFLOW_PORTS[];
extern const int NODEFLOW_NUM_TOPO;
extern const int NODEFLOW_TOPO_ORDER[];
typedef struct { const char* nodeId; size_t offset; const char* dtype; } NodeFlowInputField;
extern const int NODEFLOW_NUM_INPUT_FIELDS;
extern const NodeFlowInputField NODEFLOW_INPUT_FIELDS[];
void nodeflow_init(NodeFlowState* state);
void nodeflow_reset(NodeFlowState* state);
void nodeflow_set_input(int handle, double value, NodeFlowInputs* in, NodeFlowState* state);
double nodeflow_get_output(int handle, const NodeFlowOutputs* out, const NodeFlowState* state);
#ifdef __cplusplus
}
#endif

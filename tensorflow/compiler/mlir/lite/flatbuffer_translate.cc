/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Quant/QuantTypes.h"  // TF:llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // TF:llvm-project
#include "mlir/IR/Attributes.h"  // TF:llvm-project
#include "mlir/IR/Builders.h"  // TF:llvm-project
#include "mlir/IR/Function.h"  // TF:llvm-project
#include "mlir/IR/Location.h"  // TF:llvm-project
#include "mlir/IR/MLIRContext.h"  // TF:llvm-project
#include "mlir/IR/Module.h"  // TF:llvm-project
#include "mlir/IR/Operation.h"  // TF:llvm-project
#include "mlir/IR/StandardTypes.h"  // TF:llvm-project
#include "mlir/IR/Types.h"  // TF:llvm-project
#include "mlir/IR/Value.h"  // TF:llvm-project
#include "mlir/Support/LogicalResult.h"  // TF:llvm-project
#include "mlir/Translation.h"  // TF:llvm-project
#include "tensorflow/compiler/mlir/lite/flatbuffer_export.h"
#include "tensorflow/compiler/mlir/lite/flatbuffer_import.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/mlir_roundtrip_flags.h"

using llvm::cl::opt;

// Commandline flag to enable the control of flatbuffer import.
bool use_external_constant;

// Commandline flag to enable graph pruning.
bool experimental_prune_unreachable_nodes_unconditionally;

// NOLINTNEXTLINE
static opt<bool, true> use_external_constant_flag(
    "use-external-constant",
    llvm::cl::desc("Use external constant during flatbuffer import"),
    llvm::cl::location(use_external_constant), llvm::cl::init(false));

// TODO(b/147111261): After the importer supports generic custom ops, we should
// change the flag to a more lightwise flag, e.g.
// "import_custom_ops_as_side_effect_free_ops", and let the MLIR DCE to prune
// the operations.
// NOLINTNEXTLINE
static opt<bool, true> experimental_prune_unreachable_nodes_unconditionally_flg(
    "experimental-prune-unreachable-nodes-unconditionally",
    llvm::cl::desc("Prune nodes that are not ancestors of the output nodes."),
    llvm::cl::location(experimental_prune_unreachable_nodes_unconditionally),
    llvm::cl::init(false));

// NOLINTNEXTLINE
static opt<std::string> input_arrays_flag(
    "input-arrays",
    llvm::cl::desc(
        "List of input tensors, if different from the default inputs"),
    llvm::cl::init(""));

// NOLINTNEXTLINE
static opt<std::string> output_arrays_flag(
    "output-arrays",
    llvm::cl::desc(
        "List of output tensors, if different from the default outputs"),
    llvm::cl::init(""));
using llvm::cl::opt;

// These command line flags enable control of the translation implementation.
bool emit_builtin_tflite_ops;
bool emit_custom_ops;
bool emit_select_tf_ops;
bool lower_tensor_list_ops;
bool strip_debug_info;

// NOLINTNEXTLINE
static opt<bool, true> emit_builtin_tflite_ops_flag(
    "emit-builtin-tflite-ops",
    llvm::cl::desc(
        "Emit TFLite built in operations in the generated TFLite model"),
    llvm::cl::location(emit_builtin_tflite_ops), llvm::cl::init(true));

// NOLINTNEXTLINE
static opt<bool, true> emit_select_tf_ops_flag(
    "emit-select-tf-ops",
    llvm::cl::desc(
        "Emit Select TF operations (Flex ops) in the generated TFLite model"),
    llvm::cl::location(emit_select_tf_ops), llvm::cl::init(false));

// NOLINTNEXTLINE
static opt<bool, true> emit_custom_ops_flag(
    "emit-custom-ops",
    llvm::cl::desc("Emit Custom operations in the generated TFLite model"),
    llvm::cl::location(emit_custom_ops), llvm::cl::init(false));

// NOLINTNEXTLINE
static opt<bool, true> lower_tensor_list_ops_flag(
    "lower-tensor-list-ops",
    llvm::cl::desc("Lower the TensorList ops within the TFLite dialect"),
    llvm::cl::location(lower_tensor_list_ops), llvm::cl::init(false));

// NOLINTNEXTLINE
static opt<bool, true> strip_debug_info_flag(
    "strip-debug-info", llvm::cl::desc("Strip debug info during export"),
    llvm::cl::location(strip_debug_info), llvm::cl::init(false));

namespace mlir {
namespace {
static OwningModuleRef FlatBufferFileToMlirTrans(
    llvm::SourceMgr* source_mgr, MLIRContext* context,
    bool use_external_constant,
    bool experimental_prune_unreachable_nodes_unconditionally) {
  const llvm::MemoryBuffer* input =
      source_mgr->getMemoryBuffer(source_mgr->getMainFileID());
  std::string error;
  auto loc =
      mlir::FileLineColLoc::get(input->getBufferIdentifier(), 0, 0, context);

  // Parses input/output names from command line options.
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  // Use output parser since we only have tensor names.
  if (!tensorflow::ParseOutputArrayInfo(input_arrays_flag, &inputs).ok()) {
    return emitError(loc, "parsing input array info failed ")
               << input_arrays_flag,
           nullptr;
  }
  if (!tensorflow::ParseOutputArrayInfo(output_arrays_flag, &outputs).ok()) {
    return emitError(loc, "parsing output array info failed ")
               << output_arrays_flag,
           nullptr;
  }
  return tflite::FlatBufferToMlir(
      absl::string_view(input->getBufferStart(), input->getBufferSize()),
      context, loc, use_external_constant, inputs, outputs,
      experimental_prune_unreachable_nodes_unconditionally);
}

static LogicalResult MlirToFlatBufferFileTranslateFunction(
    ModuleOp module, llvm::raw_ostream& output) {
  std::string serialized_flatbuffer;
  std::unique_ptr<tensorflow::OpOrArgNameMapper> op_or_arg_name_mapper;
  if (strip_debug_info) {
    op_or_arg_name_mapper =
        std::make_unique<tensorflow::OpOrArgStripNameMapper>();
  } else {
    op_or_arg_name_mapper =
        std::make_unique<tensorflow::OpOrArgLocNameMapper>();
  }
  if (tflite::MlirToFlatBufferTranslateFunction(
          module, &serialized_flatbuffer, emit_builtin_tflite_ops,
          emit_select_tf_ops, emit_custom_ops, op_or_arg_name_mapper.get()))
    return mlir::failure();

  output << serialized_flatbuffer;
  return success();
}
}  // namespace

static TranslateToMLIRRegistration FlatBufferFileToMlirTransReg(
    "tflite-flatbuffer-to-mlir",
    [](llvm::SourceMgr& source_mgr, MLIRContext* context) {
      return FlatBufferFileToMlirTrans(
          &source_mgr, context, use_external_constant,
          experimental_prune_unreachable_nodes_unconditionally);
    });

static TranslateFromMLIRRegistration MLIRToFlatBufferTranslate(
    "mlir-to-tflite-flatbuffer", MlirToFlatBufferFileTranslateFunction);
}  // namespace mlir

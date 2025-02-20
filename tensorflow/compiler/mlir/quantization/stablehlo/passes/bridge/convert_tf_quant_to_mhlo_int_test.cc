/* Copyright 2023 The StableHLO Authors. All Rights Reserved.

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

#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "llvm/Support/Casting.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/DialectRegistry.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "stablehlo/dialect/ChloOps.h"  // from @stablehlo
#include "tensorflow/compiler/mlir/quantization/stablehlo/passes/bridge/passes.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/cc/constant_fold.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_dialect.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_tensor.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "xla/error_spec.h"
#include "xla/literal.h"
#include "xla/literal_util.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/pjrt/pjrt_executable.h"
#include "xla/pjrt/tfrt_cpu_pjrt_client.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/tests/literal_test_util.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace mlir::quant::stablehlo {
namespace {

class ConvertTfQuantToMhloIntTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DialectRegistry dialects;
    dialects.insert<TF::TensorFlowDialect, func::FuncDialect, chlo::ChloDialect,
                    mhlo::MhloDialect, quant::QuantizationDialect>();
    ctx_ = std::make_unique<MLIRContext>(dialects);

    // Create a CPU client with 1 device.
    TF_ASSERT_OK_AND_ASSIGN(
        pjrt_client_,
        xla::GetTfrtCpuClient(/*asynchronous=*/false, /*cpu_device_count=*/1));
    device_ = pjrt_client_->addressable_devices().front();
    CHECK(device_);
  }

  // Evaluate return value of a function using TF kernel.
  // This assumes that the module op has only 1 function and it has TF ops only.
  absl::StatusOr<std::shared_ptr<xla::Literal>> EvaluateTfFunction(
      absl::string_view program,
      absl::Span<const xla::Literal* const> arguments) {
    auto module_op = parseSourceString<ModuleOp>(program, ctx_.get());
    CHECK(module_op);
    auto func_op = llvm::dyn_cast<func::FuncOp>(
        *module_op->getBodyRegion().getOps().begin());
    if (!func_op) {
      return absl::InternalError("Input MLIR must have only 1 func");
    }
    if (arguments.size() != func_op.getNumArguments()) {
      return absl::InternalError("Input argument has wrong size");
    }

    // Convert input xla::Literal arguments to tf.Const, this allows using
    // constant folding to evaluate function return value.
    mlir::OpBuilder builder(func_op);
    for (int i = 0; i < arguments.size(); ++i) {
      const xla::Literal* const xla_literal = arguments[i];
      tensorflow::TensorShape shape;
      TF_ASSIGN_OR_RETURN(auto data_type,
                          tensorflow::EncodePrimitiveTypeAsDataType(
                              xla_literal->shape().element_type()));
      TF_RETURN_IF_ERROR(
          tensorflow::XLAShapeToTensorShape(xla_literal->shape(), &shape));
      tensorflow::Tensor tensor(data_type, shape);
      std::memcpy(static_cast<char*>(tensor.data()),
                  xla_literal->untyped_data(),
                  xla::ShapeUtil::ByteSizeOfPrimitiveType(
                      xla_literal->shape().element_type()) *
                      xla_literal->element_count());
      TF_ASSIGN_OR_RETURN(auto attrs,
                          tensorflow::ConvertTensor(tensor, &builder));
      auto cst = builder.create<TF::ConstOp>(func_op->getLoc(), attrs);
      func_op.getArgument(i).replaceAllUsesWith(cst);
    }

    // Constant fold the func.Return op's producer op to evaluate the return
    // value. The evaluation will use TF kernels.
    // This assumes that func.Return is the last op in the function and it
    // returns only 1 value.
    auto& return_op = func_op.getFunctionBody().getBlocks().back().back();
    if (!llvm::isa<func::ReturnOp>(return_op) ||
        return_op.getNumOperands() != 1) {
      return absl::InternalError(
          "Func must have ReturnOp as last op and must return 1 value");
    }
    auto def_op = return_op.getOperand(0).getDefiningOp();
    auto fold_results = ConstantFoldOpIfPossible(def_op);
    if (fold_results.size() != 1 ||
        !llvm::isa<TF::ConstOp>(fold_results[0].getDefiningOp())) {
      return absl::InternalError("Failed to evaluate TF ops");
    }

    // Convert output tensor back to xla::Literal.
    tensorflow::Tensor tensor;
    TF_RETURN_IF_ERROR(tensorflow::ConvertToTensor(
        llvm::dyn_cast<TF::ConstOp>(fold_results[0].getDefiningOp()).getValue(),
        &tensor));
    xla::Shape xla_shape;
    TF_RETURN_IF_ERROR(tensorflow::TensorShapeToXLAShape(
        tensor.dtype(), tensor.shape(), &xla_shape));
    xla::PjRtClient::HostBufferSemantics host_buffer_semantics =
        xla::PjRtClient::HostBufferSemantics::kImmutableUntilTransferCompletes;
    TF_ASSIGN_OR_RETURN(
        auto buffer,
        pjrt_client_->BufferFromHostBuffer(
            tensor.data(), xla_shape.element_type(), xla_shape.dimensions(),
            /*byte_strides=*/std::nullopt, host_buffer_semantics,
            /*on_done_with_host_buffer=*/nullptr, device_));
    return buffer->ToLiteralSync();
  }

  absl::StatusOr<std::unique_ptr<xla::PjRtLoadedExecutable>> CompileProgram(
      absl::string_view program) {
    // Parse the program.
    auto module_op = parseSourceString<ModuleOp>(program, ctx_.get());
    CHECK(module_op);
    // Run the Convert TF Quant Types, TF Quant -> MHLO Quant and MHLO Quant ->
    // MHLO int passes.
    PassManager pm(module_op->getContext());
    pm.addNestedPass<func::FuncOp>(CreateConvertTFQuantTypesPass());
    pm.addNestedPass<func::FuncOp>(CreateConvertTFQuantOpsToMHLOPass());
    pm.addNestedPass<func::FuncOp>(
        stablehlo::createConvertMHLOQuantToIntPass(false));
    CHECK(succeeded(pm.run(module_op.get())));
    // Compile the program.
    return pjrt_client_->Compile(*module_op, xla::CompileOptions{});
  }

  absl::StatusOr<std::shared_ptr<xla::Literal>>
  ExecuteProgramAndReturnSingleResult(
      xla::PjRtLoadedExecutable* executable,
      absl::Span<const xla::Literal* const> arguments) {
    // Process and buffer arguments.
    std::vector<std::unique_ptr<xla::PjRtBuffer>> buffers;
    std::vector<xla::PjRtBuffer*> buffer_ptrs;
    buffers.reserve(arguments.size());
    for (const xla::Literal* argument : arguments) {
      TF_ASSIGN_OR_RETURN(
          auto buffer, pjrt_client_->BufferFromHostLiteral(*argument, device_));
      buffer_ptrs.push_back(buffer.get());
      buffers.push_back(std::move(buffer));
    }
    // Run the executable.
    TF_ASSIGN_OR_RETURN(auto result,
                        executable->Execute({buffer_ptrs}, /*options=*/{}));
    CHECK(result.size() == 1 && result[0].size() == 1);
    return result[0][0]->ToLiteralSync();
  }

  void ExecuteAndCompareResultsWithTfKernel(
      absl::string_view program,
      absl::Span<const xla::Literal* const> arguments,
      float error_tolerance = 0.1) {
    TF_ASSERT_OK_AND_ASSIGN(auto executable, this->CompileProgram(program));

    TF_ASSERT_OK_AND_ASSIGN(
        auto result_literal,
        this->ExecuteProgramAndReturnSingleResult(executable.get(), arguments));

    TF_ASSERT_OK_AND_ASSIGN(auto expected,
                            this->EvaluateTfFunction(program, arguments));
    EXPECT_TRUE(xla::LiteralTestUtil::Near(*expected, *result_literal,
                                           xla::ErrorSpec(error_tolerance)));
  }

  std::unique_ptr<MLIRContext> ctx_;
  std::unique_ptr<xla::PjRtClient> pjrt_client_;
  xla::PjRtDevice* device_;
};

TEST_F(ConvertTfQuantToMhloIntTest, UniformQuantizeAndDequantize) {
  constexpr absl::string_view kProgram = R"mlir(
func.func @main(%arg0: tensor<4xf32>) -> tensor<4xf32> {
  %scale = "tf.Const"() { value = dense<10.0> : tensor<f32> } : ()
    -> tensor<f32>
  %zp = "tf.Const"() { value = dense<3> : tensor<i32> } : () -> tensor<i32>
  %0 = "tf.UniformQuantize"(%arg0, %scale, %zp) {
    quantization_axis = -1 : i64,
    quantization_min_val = -128 : i64,
    quantization_max_val = 127 : i64
  } : (tensor<4xf32>, tensor<f32>, tensor<i32>) -> tensor<4x!tf_type.qint8>
  %1 = "tf.UniformDequantize"(%0, %scale, %zp) {
    quantization_axis = -1 : i64,
    quantization_min_val = -128 : i64,
    quantization_max_val = 127 : i64
  } : (tensor<4x!tf_type.qint8>, tensor<f32>, tensor<i32>) -> tensor<4xf32>
  return %1 : tensor<4xf32>
})mlir";
  auto arg0 =
      xla::LiteralUtil::CreateR1<float>({100.0f, 20000.0f, -2409.0f, -25.1f});
  ExecuteAndCompareResultsWithTfKernel(kProgram, {&arg0});
}

TEST_F(ConvertTfQuantToMhloIntTest, UniformQuantizeConvolution) {
  constexpr absl::string_view kProgram = R"mlir(
func.func @main(%input: tensor<1x2x2x1xf32>, %filter: tensor<2x1x1x1xf32>) -> tensor<1x2x2x1xf32> {
    %input_scale = "tf.Const"() { value = dense<7.3> : tensor<f32> } : ()
    -> tensor<f32>
    %input_zp = "tf.Const"() { value = dense<-45> : tensor<i32> } : () -> tensor<i32>
    %filter_scale = "tf.Const"() { value = dense<0.047> : tensor<f32> } : ()
    -> tensor<f32>
    %filter_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %accum_scale = "tf.Const"() { value = dense<0.3431> : tensor<f32> } : ()
    -> tensor<f32>
    %accum_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %quant_input = "tf.UniformQuantize"(%input, %input_scale, %input_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8",
      attr_map = "", quantization_axis = -1 : i64, quantization_max_val = 127 : i64,
      quantization_min_val = -128 : i64
    } : (tensor<1x2x2x1xf32>, tensor<f32>, tensor<i32>) -> tensor<1x2x2x1x!tf_type.qint8>
    %quant_filter = "tf.UniformQuantize"(%filter, %filter_scale, %filter_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8",
      attr_map = "", quantization_axis = -1 : i64,
      quantization_max_val = 127 : i64, quantization_min_val = -128 : i64
    } : (tensor<2x1x1x1xf32>, tensor<f32>, tensor<i32>) -> tensor<2x1x1x1x!tf_type.qint8>
    %0 = "tf.UniformQuantizedConvolution"(
      %quant_input, %quant_filter, %input_scale, %input_zp,
      %filter_scale, %filter_zp, %accum_scale, %accum_zp
    ) {
      Tin = "tfdtype$DT_QINT8", Tout = "tfdtype$DT_QINT32",
      attr_map = "", batch_group_count = 1 : i64,
      dimension_numbers = "\10\03\1A\02\01\02 \02(\032\02\00\01@\03J\02\01\02",
      explicit_padding = [], feature_group_count = 1 : i64, lhs_dilation = [1, 1],
      lhs_quantization_axis = -1 : i64, lhs_quantization_max_val = 127 : i64,
      lhs_quantization_min_val = -128 : i64, output_quantization_axis = -1 : i64,
      output_quantization_max_val = 2147483647 : i64,
      output_quantization_min_val = -2147483648 : i64, padding = "SAME",
      rhs_dilation = [1, 1], rhs_quantization_axis = -1 : i64,
      rhs_quantization_max_val = 127 : i64, rhs_quantization_min_val = -128 : i64,
      window_strides = [1, 1]
    } : (tensor<1x2x2x1x!tf_type.qint8>, tensor<2x1x1x1x!tf_type.qint8>,
      tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>
    ) -> tensor<1x2x2x1x!tf_type.qint32>
    %output = "tf.UniformDequantize"(%0, %accum_scale, %accum_zp) {
      quantization_axis = -1 : i64, quantization_min_val = -128 : i64,
      quantization_max_val = 127 : i64
    } : (tensor<1x2x2x1x!tf_type.qint32>, tensor<f32>, tensor<i32>) -> tensor<1x2x2x1xf32>
    return %output : tensor<1x2x2x1xf32>
})mlir";
  auto input = xla::LiteralUtil::CreateR4<float>(
      {{{{14.f}, {-100.f}}, {{-200.f}, {350.f}}}});
  auto filter = xla::LiteralUtil::CreateR4<float>({{{{4.1f}}}, {{{-2.f}}}});
  ExecuteAndCompareResultsWithTfKernel(kProgram, {&input, &filter});
}

TEST_F(ConvertTfQuantToMhloIntTest, UniformQuantizeConvolutionHybrid) {
  constexpr absl::string_view kProgram = R"mlir(
func.func @main(%input: tensor<1x2x2x1xf32>, %filter: tensor<2x1x1x1xf32>) -> tensor<1x2x2x1xf32> {
    %filter_scale = "tf.Const"() { value = dense<0.047> : tensor<f32> } : ()
    -> tensor<f32>
    %filter_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %quant_filter = "tf.UniformQuantize"(%filter, %filter_scale, %filter_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8",
      attr_map = "", quantization_axis = -1 : i64,
      quantization_max_val = 127 : i64, quantization_min_val = -128 : i64
    } : (tensor<2x1x1x1xf32>, tensor<f32>, tensor<i32>) -> tensor<2x1x1x1x!tf_type.qint8>
    %0 = "tf.UniformQuantizedConvolutionHybrid"(
      %input, %quant_filter, %filter_scale, %filter_zp
    ) {
      Tin = "tfdtype$DT_QINT8", Tout = "tfdtype$DT_FLOAT",
      attr_map = "", batch_group_count = 1 : i64,
      dimension_numbers = "\10\03\1A\02\01\02 \02(\032\02\00\01@\03J\02\01\02",
      explicit_padding = [], feature_group_count = 1 : i64, lhs_dilation = [1, 1],
      padding = "SAME", rhs_dilation = [1, 1], rhs_quantization_axis = -1 : i64,
      rhs_quantization_max_val = 127 : i64, rhs_quantization_min_val = -128 : i64,
      window_strides = [1, 1]
    } : (tensor<1x2x2x1xf32>, tensor<2x1x1x1x!tf_type.qint8>,
      tensor<f32>, tensor<i32>) -> tensor<1x2x2x1xf32>
    return %0 : tensor<1x2x2x1xf32>
})mlir";
  auto input = xla::LiteralUtil::CreateR4<float>(
      {{{{14.f}, {-100.f}}, {{-200.f}, {350.f}}}});
  auto filter = xla::LiteralUtil::CreateR4<float>({{{{4.1f}}}, {{{-2.f}}}});
  // The large tolerance here is expected because
  // tf.UniformQuantizedConvolutionHybrid does DRQ. But StableHLO hybrid ops
  // does weight-only.
  ExecuteAndCompareResultsWithTfKernel(kProgram, {&input, &filter},
                                       /*error_tolerance=*/5.0);
}

TEST_F(ConvertTfQuantToMhloIntTest, UniformQuantizeDot) {
  constexpr absl::string_view kProgram = R"mlir(
func.func @main(%input: tensor<1x2xf32>, %filter: tensor<2x3xf32>) -> tensor<1x3xf32> {
    %input_scale = "tf.Const"() { value = dense<0.588> : tensor<f32> } : ()
    -> tensor<f32>
    %input_zp = "tf.Const"() { value = dense<42> : tensor<i32> } : () -> tensor<i32>
    %filter_scale = "tf.Const"() { value = dense<0.0235> : tensor<f32> } : ()
    -> tensor<f32>
    %filter_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %accum_scale = "tf.Const"() { value = dense<0.013818> : tensor<f32> } : ()
    -> tensor<f32>
    %accum_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %quant_input = "tf.UniformQuantize"(%input, %input_scale, %input_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8", attr_map = "",
      quantization_axis = -1 : i64, quantization_max_val = 127 : i64,
      quantization_min_val = -128 : i64
    } : (tensor<1x2xf32>, tensor<f32>, tensor<i32>) -> tensor<1x2x!tf_type.qint8>
    %quant_filter = "tf.UniformQuantize"(%filter, %filter_scale, %filter_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8", attr_map = "",
      quantization_axis = -1 : i64, quantization_max_val = 127 : i64,
      quantization_min_val = -128 : i64
    } : (tensor<2x3xf32>, tensor<f32>, tensor<i32>) -> tensor<2x3x!tf_type.qint8>
    %0 = "tf.UniformQuantizedDot"(
      %quant_input, %quant_filter, %input_scale, %input_zp, %filter_scale,
      %filter_zp, %accum_scale, %accum_zp
    ) {
      Tin = "tfdtype$DT_QINT8", Tout = "tfdtype$DT_QINT32", attr_map = "",
      device = "", lhs_quantization_axis = -1 : i64,
      lhs_quantization_max_val = 127 : i64, lhs_quantization_min_val = -128 : i64,
      output_quantization_axis = -1 : i64, output_quantization_max_val = 2147483647 : i64,
      output_quantization_min_val = -2147483648 : i64, rhs_quantization_axis = -1 : i64,
      rhs_quantization_max_val = 127 : i64, rhs_quantization_min_val = -128 : i64
    } : (
      tensor<1x2x!tf_type.qint8>, tensor<2x3x!tf_type.qint8>, tensor<f32>,
      tensor<i32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>
    ) -> tensor<1x3x!tf_type.qint32>
    %output = "tf.UniformDequantize"(%0, %accum_scale, %accum_zp) {
      quantization_axis = -1 : i64, quantization_min_val = -128 : i64,
      quantization_max_val = 127 : i64
    } : (tensor<1x3x!tf_type.qint32>, tensor<f32>, tensor<i32>) -> tensor<1x3xf32>
    return %output : tensor<1x3xf32>
})mlir";
  auto input = xla::LiteralUtil::CreateR2<float>({{50.f, -100.f}});
  auto filter =
      xla::LiteralUtil::CreateR2<float>({{1.f, 2.f, 3.f}, {-1.f, -3.f, 1.f}});
  ExecuteAndCompareResultsWithTfKernel(kProgram, {&input, &filter});
}

TEST_F(ConvertTfQuantToMhloIntTest, UniformQuantizeDotHybrid) {
  constexpr absl::string_view kProgram = R"mlir(
func.func @main(%input: tensor<1x2xf32>, %filter: tensor<2x3xf32>) -> tensor<1x3xf32> {
    %filter_scale = "tf.Const"() { value = dense<0.0235> : tensor<f32> } : ()
    -> tensor<f32>
    %filter_zp = "tf.Const"() { value = dense<0> : tensor<i32> } : () -> tensor<i32>
    %quant_filter = "tf.UniformQuantize"(%filter, %filter_scale, %filter_zp) {
      Tin = "tfdtype$DT_FLOAT", Tout = "tfdtype$DT_QINT8", attr_map = "",
      quantization_axis = -1 : i64, quantization_max_val = 127 : i64,
      quantization_min_val = -128 : i64
    } : (tensor<2x3xf32>, tensor<f32>, tensor<i32>) -> tensor<2x3x!tf_type.qint8>
    %0 = "tf.UniformQuantizedDotHybrid"(
      %input, %quant_filter, %filter_scale, %filter_zp
    ) {
      Tin = "tfdtype$DT_QINT8", Tout = "tfdtype$DT_FLOAT", attr_map = "",
      device = "", rhs_quantization_axis = -1 : i64,
      rhs_quantization_max_val = 127 : i64, rhs_quantization_min_val = -128 : i64
    } : (tensor<1x2xf32>, tensor<2x3x!tf_type.qint8>, tensor<f32>, tensor<i32>) -> tensor<1x3xf32>
    return %0 : tensor<1x3xf32>
})mlir";
  auto input = xla::LiteralUtil::CreateR2<float>({{50.f, -100.f}});
  auto filter =
      xla::LiteralUtil::CreateR2<float>({{1.f, 2.f, 3.f}, {-1.f, -3.f, 1.f}});
  ExecuteAndCompareResultsWithTfKernel(kProgram, {&input, &filter});
}

}  // namespace
}  // namespace mlir::quant::stablehlo

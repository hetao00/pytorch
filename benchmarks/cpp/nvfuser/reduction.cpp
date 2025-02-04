#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/executor.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/all_schedulers.h>

#include <benchmark/benchmark.h>

#include <cuda_runtime.h>

#include <sstream>

#include "utils.h"

using namespace torch::jit::fuser::cuda;

// Return reduction tensor view and output of reduction
static void setupReduction(Fusion* fusion, DataType dtype, int red_axis) {
  FusionGuard fg(fusion);

  bool is_fp16 = dtype == DataType::Half;

  TensorView* tv0 = makeContigTensor(2, dtype);
  fusion->addInput(tv0);

  TensorView* tv0_cast = tv0;
  if (is_fp16) {
    tv0_cast = castOp(DataType::Float, tv0);
  }

  TensorView* tv1 = sum(tv0_cast, {red_axis});

  TensorView* tv1_cast = tv1;
  if (is_fp16) {
    tv1_cast = castOp(DataType::Half, tv1);
  }

  fusion->addOutput(tv1_cast);

  TensorView* output_of_reduction = nullptr;
  if (is_fp16) {
    output_of_reduction = tv1_cast;
  }
}

static void NvFuserScheduler_Reduction(
    benchmark::State& benchmark_state,
    FusionExecutorCache* fusion_executor_cache,
    DataType dtype,
    int reduction_dim) {
  auto reduction_size = benchmark_state.range(0);
  auto iter_size = benchmark_state.range(1);

  at::manual_seed(0);
  auto options =
      at::TensorOptions().dtype(data_type_to_aten(dtype)).device(at::kCUDA, 0);
  at::Tensor aten_input =
      (reduction_dim ? at::randn({iter_size, reduction_size}, options)
                     : at::randn({reduction_size, iter_size}, options));

  fusion_executor_cache->profile(true);
  fusion_executor_cache->runFusionWithInputs({aten_input});

  auto compile_log = fusion_executor_cache->getMostRecentExecutorInfo();
  auto executor_instance = compile_log.fusion_executor;
  TORCH_INTERNAL_ASSERT(compile_log.reduction_params.has_value());
  TORCH_INTERNAL_ASSERT(compile_log.launch_constraints.has_value());
  auto rparams = toString(compile_log.reduction_params.value());
  auto lparams = toString(compile_log.launch_constraints.value());

  benchmark_state.SetLabel(rparams + lparams);

  fusion_executor_cache->profile(false);
  executor_instance->setMeasureKernelTimeFlag(true);
  // Sync everything up before we start
  cudaDeviceSynchronize();
  for (auto _ : benchmark_state) {
    auto cg_outputs = fusion_executor_cache->runFusionWithInputs({aten_input});
    benchmark_state.SetIterationTime(
        executor_instance->kernelTimeMs() / 1000.0);
    clearL2Cache();
  }
  // Sync everything up before we're finished, don't want to run ahead on the
  // cpu while benchmarking.
  cudaDeviceSynchronize();

  benchmark_state.SetBytesProcessed(
      int64_t(benchmark_state.iterations()) *
      (iter_size * reduction_size + iter_size) * int64_t(dataTypeSize(dtype)));
}

NVFUSER_BENCHMARK_DEFINE(
    NvFuserScheduler_Reduction_Outer_fp32,
    setupReduction,
    NvFuserScheduler_Reduction,
    DataType::Float,
    0);
NVFUSER_BENCHMARK_DEFINE(
    NvFuserScheduler_Reduction_Outer_fp16,
    setupReduction,
    NvFuserScheduler_Reduction,
    DataType::Half,
    0);
NVFUSER_BENCHMARK_DEFINE(
    NvFuserScheduler_Reduction_Inner_fp32,
    setupReduction,
    NvFuserScheduler_Reduction,
    DataType::Float,
    1);
NVFUSER_BENCHMARK_DEFINE(
    NvFuserScheduler_Reduction_Inner_fp16,
    setupReduction,
    NvFuserScheduler_Reduction,
    DataType::Half,
    1);

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp32)
    ->RangeMultiplier(8)
    ->Ranges({{1, 1024 * 1024}, {160, 320}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp32)
    ->RangeMultiplier(4)
    ->Ranges({{32768, 128 * 1024 * 1024}, {2, 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp32)
    ->RangeMultiplier(4)
    ->Ranges({{2, 16}, {32768, 128 * 1024 * 1024}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp32)
    ->RangeMultiplier(2)
    ->Ranges({{128, 1024 * 16}, {128, 1024 * 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp16)
    ->RangeMultiplier(8)
    ->Ranges({{1, 1024 * 1024}, {160, 320}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp16)
    ->RangeMultiplier(4)
    ->Ranges({{32768, 128 * 1024 * 1024}, {2, 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp16)
    ->RangeMultiplier(4)
    ->Ranges({{2, 16}, {32768, 128 * 1024 * 1024}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Outer_fp16)
    ->RangeMultiplier(2)
    ->Ranges({{128, 1024 * 16}, {128, 1024 * 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp32)
    ->RangeMultiplier(8)
    ->Ranges({{1, 1024 * 1024}, {160, 320}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp32)
    ->RangeMultiplier(4)
    ->Ranges({{32768, 128 * 1024 * 1024}, {2, 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp32)
    ->RangeMultiplier(4)
    ->Ranges({{2, 16}, {32768, 128 * 1024 * 1024}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp32)
    ->RangeMultiplier(2)
    ->Ranges({{128, 1024 * 16}, {128, 1024 * 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp16)
    ->RangeMultiplier(8)
    ->Ranges({{1, 1024 * 1024}, {160, 320}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp16)
    ->RangeMultiplier(4)
    ->Ranges({{32768, 128 * 1024 * 1024}, {2, 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp16)
    ->RangeMultiplier(4)
    ->Ranges({{2, 16}, {32768, 128 * 1024 * 1024}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

NVFUSER_BENCHMARK_RUN(NvFuserScheduler_Reduction_Inner_fp16)
    ->RangeMultiplier(2)
    ->Ranges({{128, 1024 * 16}, {128, 1024 * 16}})
    ->Unit(benchmark::kMicrosecond)
    ->UseManualTime();

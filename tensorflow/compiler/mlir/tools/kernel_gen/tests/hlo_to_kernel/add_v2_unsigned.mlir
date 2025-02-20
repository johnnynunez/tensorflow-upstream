// RUN: hlo_to_kernel --input=%s --output=%t --unroll_factors=4 --tile_sizes=256 --arch=gfx906

func.func @AddV2(%arg0: tensor<*xui32>, %arg1: tensor<*xui32>)
    -> tensor<*xui32> attributes {tf_entry, llvm.emit_c_interface} {
  %0 = chlo.broadcast_add %arg0, %arg1 : (tensor<*xui32>, tensor<*xui32>) -> tensor<*xui32>
  return %0 : tensor<*xui32>
}

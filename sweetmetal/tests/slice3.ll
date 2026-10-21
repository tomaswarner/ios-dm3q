; sweetmetal slice 3 test shaders, hand-written AIR (no Metal toolchain here), after
; runtime_examples.ll. Metal source equivalent:
;   struct In { float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
;   struct Out { float4 position [[position]]; float4 color; };
;   vertex Out attr_vertex(In in [[stage_in]]) { return { float4(in.position, 1), in.color }; }
;   fragment float4 attr_fragment(Out in [[stage_in]]) { return in.color; }
source_filename = "-"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define <{ <4 x float>, <4 x float> }> @attr_vertex(<3 x float> noundef %0, <4 x float> noundef %1) local_unnamed_addr #0 {
  %3 = shufflevector <3 x float> %0, <3 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %4 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %3, 0
  %5 = insertvalue <{ <4 x float>, <4 x float> }> %4, <4 x float> %1, 1
  ret <{ <4 x float>, <4 x float> }> %5
}

define <4 x float> @attr_fragment(<4 x float> %0, <4 x float> returned %1) local_unnamed_addr #0 {
  ret <4 x float> %1
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.vertex = !{!9}
!air.fragment = !{!17}
!air.compile_options = !{!24, !25, !26}
!llvm.ident = !{!27}
!air.version = !{!28}
!air.language_version = !{!29}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{ptr @attr_vertex, !10, !13}
!10 = !{!11, !12}
!11 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!12 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!13 = !{!14, !15}
!14 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float3", !"air.arg_name", !"position"}
!15 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!17 = !{ptr @attr_fragment, !18, !20}
!18 = !{!19}
!19 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!20 = !{!21, !22}
!21 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position", !"air.arg_unused"}
!22 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!24 = !{!"air.compile.denorms_disable"}
!25 = !{!"air.compile.fast_math_enable"}
!26 = !{!"air.compile.framebuffer_fetch_enable"}
!27 = !{!"Apple metal version 32023.921 (metalfe-32023.921.6)"}
!28 = !{i32 2, i32 8, i32 0}
!29 = !{!"Metal", i32 4, i32 0, i32 0}

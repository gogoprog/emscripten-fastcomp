; RUN: pnacl-llc -mtriple=armv7-unknown-nacl -mattr=+neon -filetype=obj %s -o - \
; RUN:  | llvm-objdump -disassemble -triple armv7 - | FileCheck %s

%struct.__neon_int8x8x2_t = type { <8 x i8>,  <8 x i8> }
%struct.__neon_int16x4x2_t = type { <4 x i16>, <4 x i16> }
%struct.__neon_int32x2x2_t = type { <2 x i32>, <2 x i32> }
%struct.__neon_float32x2x2_t = type { <2 x float>, <2 x float> }

%struct.__neon_int16x8x2_t = type { <8 x i16>, <8 x i16> }
%struct.__neon_int32x4x2_t = type { <4 x i32>, <4 x i32> }
%struct.__neon_float32x4x2_t = type { <4 x float>, <4 x float> }

declare %struct.__neon_int8x8x2_t @llvm.arm.neon.vld2lane.v8i8(i8*, <8 x i8>, <8 x i8>, i32, i32) nounwind readonly
declare %struct.__neon_int16x4x2_t @llvm.arm.neon.vld2lane.v4i16(i8*, <4 x i16>, <4 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x2x2_t @llvm.arm.neon.vld2lane.v2i32(i8*, <2 x i32>, <2 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x2x2_t @llvm.arm.neon.vld2lane.v2f32(i8*, <2 x float>, <2 x float>, i32, i32) nounwind readonly

declare %struct.__neon_int16x8x2_t @llvm.arm.neon.vld2lane.v8i16(i8*, <8 x i16>, <8 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x4x2_t @llvm.arm.neon.vld2lane.v4i32(i8*, <4 x i32>, <4 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x4x2_t @llvm.arm.neon.vld2lane.v4f32(i8*, <4 x float>, <4 x float>, i32, i32) nounwind readonly

%struct.__neon_int8x8x3_t = type { <8 x i8>,  <8 x i8>,  <8 x i8> }
%struct.__neon_int16x4x3_t = type { <4 x i16>, <4 x i16>, <4 x i16> }
%struct.__neon_int32x2x3_t = type { <2 x i32>, <2 x i32>, <2 x i32> }
%struct.__neon_float32x2x3_t = type { <2 x float>, <2 x float>, <2 x float> }

%struct.__neon_int16x8x3_t = type { <8 x i16>, <8 x i16>, <8 x i16> }
%struct.__neon_int32x4x3_t = type { <4 x i32>, <4 x i32>, <4 x i32> }
%struct.__neon_float32x4x3_t = type { <4 x float>, <4 x float>, <4 x float> }

declare %struct.__neon_int8x8x3_t @llvm.arm.neon.vld3lane.v8i8(i8*, <8 x i8>, <8 x i8>, <8 x i8>, i32, i32) nounwind readonly
declare %struct.__neon_int16x4x3_t @llvm.arm.neon.vld3lane.v4i16(i8*, <4 x i16>, <4 x i16>, <4 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x2x3_t @llvm.arm.neon.vld3lane.v2i32(i8*, <2 x i32>, <2 x i32>, <2 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x2x3_t @llvm.arm.neon.vld3lane.v2f32(i8*, <2 x float>, <2 x float>, <2 x float>, i32, i32) nounwind readonly

declare %struct.__neon_int16x8x3_t @llvm.arm.neon.vld3lane.v8i16(i8*, <8 x i16>, <8 x i16>, <8 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x4x3_t @llvm.arm.neon.vld3lane.v4i32(i8*, <4 x i32>, <4 x i32>, <4 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x4x3_t @llvm.arm.neon.vld3lane.v4f32(i8*, <4 x float>, <4 x float>, <4 x float>, i32, i32) nounwind readonly

%struct.__neon_int8x8x4_t = type { <8 x i8>,  <8 x i8>,  <8 x i8>,  <8 x i8> }
%struct.__neon_int16x4x4_t = type { <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16> }
%struct.__neon_int32x2x4_t = type { <2 x i32>, <2 x i32>, <2 x i32>, <2 x i32> }
%struct.__neon_float32x2x4_t = type { <2 x float>, <2 x float>, <2 x float>, <2 x float> }

%struct.__neon_int16x8x4_t = type { <8 x i16>, <8 x i16>, <8 x i16>, <8 x i16> }
%struct.__neon_int32x4x4_t = type { <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32> }
%struct.__neon_float32x4x4_t = type { <4 x float>, <4 x float>, <4 x float>, <4 x float> }

declare %struct.__neon_int8x8x4_t @llvm.arm.neon.vld4lane.v8i8(i8*, <8 x i8>, <8 x i8>, <8 x i8>, <8 x i8>, i32, i32) nounwind readonly
declare %struct.__neon_int16x4x4_t @llvm.arm.neon.vld4lane.v4i16(i8*, <4 x i16>, <4 x i16>, <4 x i16>, <4 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x2x4_t @llvm.arm.neon.vld4lane.v2i32(i8*, <2 x i32>, <2 x i32>, <2 x i32>, <2 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x2x4_t @llvm.arm.neon.vld4lane.v2f32(i8*, <2 x float>, <2 x float>, <2 x float>, <2 x float>, i32, i32) nounwind readonly

declare %struct.__neon_int16x8x4_t @llvm.arm.neon.vld4lane.v8i16(i8*, <8 x i16>, <8 x i16>, <8 x i16>, <8 x i16>, i32, i32) nounwind readonly
declare %struct.__neon_int32x4x4_t @llvm.arm.neon.vld4lane.v4i32(i8*, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, i32, i32) nounwind readonly
declare %struct.__neon_float32x4x4_t @llvm.arm.neon.vld4lane.v4f32(i8*, <4 x float>, <4 x float>, <4 x float>, <4 x float>, i32, i32) nounwind readonly

define <8 x i8> @vld1lanei8(i8* %A, <8 x i8>* %B) nounwind {
  %tmp1 = load <8 x i8>, <8 x i8>* %B
  %tmp2 = load i8, i8* %A, align 8
  %tmp3 = insertelement <8 x i8> %tmp1, i8 %tmp2, i32 3
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.8 {{{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <8 x i8> %tmp3
}

define <4 x i16> @vld1lanei16(i16* %A, <4 x i16>* %B) nounwind {
  %tmp1 = load <4 x i16>, <4 x i16>* %B
  %tmp2 = load i16, i16* %A, align 8
  %tmp3 = insertelement <4 x i16> %tmp1, i16 %tmp2, i32 2
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.16 {{{d[0-9]+\[[0-9]\]}}}, [r0:16]
  ret <4 x i16> %tmp3
}

define <2 x i32> @vld1lanei32(i32* %A, <2 x i32>* %B) nounwind {
  %tmp1 = load <2 x i32>, <2 x i32>* %B
  %tmp2 = load i32, i32* %A, align 8
  %tmp3 = insertelement <2 x i32> %tmp1, i32 %tmp2, i32 1
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.32 {{{d[0-9]+\[[0-9]\]}}}, [r0:32]
  ret <2 x i32> %tmp3
}

define <16 x i8> @vld1laneQi8(i8* %A, <16 x i8>* %B) nounwind {
  %tmp1 = load <16 x i8>, <16 x i8>* %B
  %tmp2 = load i8, i8* %A, align 8
  %tmp3 = insertelement <16 x i8> %tmp1, i8 %tmp2, i32 9
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.8 {{{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <16 x i8> %tmp3
}

define <8 x i16> @vld1laneQi16(i16* %A, <8 x i16>* %B) nounwind {
  %tmp1 = load <8 x i16>, <8 x i16>* %B
  %tmp2 = load i16, i16* %A, align 8
  %tmp3 = insertelement <8 x i16> %tmp1, i16 %tmp2, i32 5
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.16 {{{d[0-9]+\[[0-9]\]}}}, [r0:16]
  ret <8 x i16> %tmp3
}

define <4 x i32> @vld1laneQi32(i32* %A, <4 x i32>* %B) nounwind {
  %tmp1 = load <4 x i32>, <4 x i32>* %B
  %tmp2 = load i32, i32* %A, align 8
  %tmp3 = insertelement <4 x i32> %tmp1, i32 %tmp2, i32 3
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld1.32 {{{d[0-9]+\[[0-9]\]}}}, [r0:32]
  ret <4 x i32> %tmp3
}

define <8 x i8> @vld2lanei8(i8* %A, <8 x i8>* %B) nounwind {
  %tmp1 = load <8 x i8>, <8 x i8>* %B
  %tmp2 = call %struct.__neon_int8x8x2_t @llvm.arm.neon.vld2lane.v8i8(i8* %A, <8 x i8> %tmp1, <8 x i8> %tmp1, i32 1, i32 4)
  %tmp3 = extractvalue %struct.__neon_int8x8x2_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int8x8x2_t %tmp2, 1
  %tmp5 = add <8 x i8> %tmp3, %tmp4
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld2.8 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:16]
  ret <8 x i8> %tmp5
}

define <4 x i16> @vld2lanei16(i16* %A, <4 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <4 x i16>, <4 x i16>* %B
  %tmp2 = call %struct.__neon_int16x4x2_t @llvm.arm.neon.vld2lane.v4i16(i8* %tmp0, <4 x i16> %tmp1, <4 x i16> %tmp1, i32 1, i32 8)
  %tmp3 = extractvalue %struct.__neon_int16x4x2_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x4x2_t %tmp2, 1
  %tmp5 = add <4 x i16> %tmp3, %tmp4
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld2.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:32]
  ret <4 x i16> %tmp5
}

define <2 x i32> @vld2lanei32(i32 %foo, i32 %bar, i32 %baz,
                              i32* %A, <2 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <2 x i32>, <2 x i32>* %B
  %tmp2 = call %struct.__neon_int32x2x2_t @llvm.arm.neon.vld2lane.v2i32(i8* %tmp0, <2 x i32> %tmp1, <2 x i32> %tmp1, i32 1, i32 1)
  %tmp3 = extractvalue %struct.__neon_int32x2x2_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x2x2_t %tmp2, 1
  %tmp5 = add <2 x i32> %tmp3, %tmp4
; CHECK:         bic r3, r3, #-1073741824
; CHECK-NEXT:    vld2.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r3]
  ret <2 x i32> %tmp5
}

define <8 x i16> @vld2laneQi16(i16* %A, <8 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <8 x i16>, <8 x i16>* %B
  %tmp2 = call %struct.__neon_int16x8x2_t @llvm.arm.neon.vld2lane.v8i16(i8* %tmp0, <8 x i16> %tmp1, <8 x i16> %tmp1, i32 5, i32 1)
  %tmp3 = extractvalue %struct.__neon_int16x8x2_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x8x2_t %tmp2, 1
  %tmp5 = add <8 x i16> %tmp3, %tmp4
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld2.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <8 x i16> %tmp5
}

define <4 x i32> @vld2laneQi32(i32* %A, <4 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <4 x i32>, <4 x i32>* %B
  %tmp2 = call %struct.__neon_int32x4x2_t @llvm.arm.neon.vld2lane.v4i32(i8* %tmp0, <4 x i32> %tmp1, <4 x i32> %tmp1, i32 2, i32 16)
  %tmp3 = extractvalue %struct.__neon_int32x4x2_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x4x2_t %tmp2, 1
  %tmp5 = add <4 x i32> %tmp3, %tmp4
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld2.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:64]
  ret <4 x i32> %tmp5
}

define <8 x i8> @vld3lanei8(i8* %A, <8 x i8>* %B) nounwind {
  %tmp1 = load <8 x i8>, <8 x i8>* %B
  %tmp2 = call %struct.__neon_int8x8x3_t @llvm.arm.neon.vld3lane.v8i8(i8* %A, <8 x i8> %tmp1, <8 x i8> %tmp1, <8 x i8> %tmp1, i32 1, i32 1)
  %tmp3 = extractvalue %struct.__neon_int8x8x3_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int8x8x3_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int8x8x3_t %tmp2, 2
  %tmp6 = add <8 x i8> %tmp3, %tmp4
  %tmp7 = add <8 x i8> %tmp5, %tmp6
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld3.8 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <8 x i8> %tmp7
}

define <4 x i16> @vld3lanei16(i16* %A, <4 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <4 x i16>, <4 x i16>* %B
  %tmp2 = call %struct.__neon_int16x4x3_t @llvm.arm.neon.vld3lane.v4i16(i8* %tmp0, <4 x i16> %tmp1, <4 x i16> %tmp1, <4 x i16> %tmp1, i32 1, i32 8)
  %tmp3 = extractvalue %struct.__neon_int16x4x3_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x4x3_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int16x4x3_t %tmp2, 2
  %tmp6 = add <4 x i16> %tmp3, %tmp4
  %tmp7 = add <4 x i16> %tmp5, %tmp6
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld3.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <4 x i16> %tmp7
}

define <2 x i32> @vld3lanei32(i32* %A, <2 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <2 x i32>, <2 x i32>* %B
  %tmp2 = call %struct.__neon_int32x2x3_t @llvm.arm.neon.vld3lane.v2i32(i8* %tmp0, <2 x i32> %tmp1, <2 x i32> %tmp1, <2 x i32> %tmp1, i32 1, i32 1)
  %tmp3 = extractvalue %struct.__neon_int32x2x3_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x2x3_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int32x2x3_t %tmp2, 2
  %tmp6 = add <2 x i32> %tmp3, %tmp4
  %tmp7 = add <2 x i32> %tmp5, %tmp6
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld3.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <2 x i32> %tmp7
}

define <8 x i16> @vld3laneQi16(i16* %A, <8 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <8 x i16>, <8 x i16>* %B
  %tmp2 = call %struct.__neon_int16x8x3_t @llvm.arm.neon.vld3lane.v8i16(i8* %tmp0, <8 x i16> %tmp1, <8 x i16> %tmp1, <8 x i16> %tmp1, i32 1, i32 8)
  %tmp3 = extractvalue %struct.__neon_int16x8x3_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x8x3_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int16x8x3_t %tmp2, 2
  %tmp6 = add <8 x i16> %tmp3, %tmp4
  %tmp7 = add <8 x i16> %tmp5, %tmp6
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld3.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <8 x i16> %tmp7
}

define <4 x i32> @vld3laneQi32(i32* %A, <4 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <4 x i32>, <4 x i32>* %B
  %tmp2 = call %struct.__neon_int32x4x3_t @llvm.arm.neon.vld3lane.v4i32(i8* %tmp0, <4 x i32> %tmp1, <4 x i32> %tmp1, <4 x i32> %tmp1, i32 3, i32 1)
  %tmp3 = extractvalue %struct.__neon_int32x4x3_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x4x3_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int32x4x3_t %tmp2, 2
  %tmp6 = add <4 x i32> %tmp3, %tmp4
  %tmp7 = add <4 x i32> %tmp5, %tmp6
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld3.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <4 x i32> %tmp7
}

define <8 x i8> @vld4lanei8(i8* %A, <8 x i8>* %B) nounwind {
  %tmp1 = load <8 x i8>, <8 x i8>* %B
  %tmp2 = call %struct.__neon_int8x8x4_t @llvm.arm.neon.vld4lane.v8i8(i8* %A, <8 x i8> %tmp1, <8 x i8> %tmp1, <8 x i8> %tmp1, <8 x i8> %tmp1, i32 1, i32 8)
  %tmp3 = extractvalue %struct.__neon_int8x8x4_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int8x8x4_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int8x8x4_t %tmp2, 2
  %tmp6 = extractvalue %struct.__neon_int8x8x4_t %tmp2, 3
  %tmp7 = add <8 x i8> %tmp3, %tmp4
  %tmp8 = add <8 x i8> %tmp5, %tmp6
  %tmp9 = add <8 x i8> %tmp7, %tmp8
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld4.8 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:32]
  ret <8 x i8> %tmp9
}

define <4 x i16> @vld4lanei16(i16* %A, <4 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <4 x i16>, <4 x i16>* %B
  %tmp2 = call %struct.__neon_int16x4x4_t @llvm.arm.neon.vld4lane.v4i16(i8* %tmp0, <4 x i16> %tmp1, <4 x i16> %tmp1, <4 x i16> %tmp1, <4 x i16> %tmp1, i32 1, i32 4)
  %tmp3 = extractvalue %struct.__neon_int16x4x4_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x4x4_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int16x4x4_t %tmp2, 2
  %tmp6 = extractvalue %struct.__neon_int16x4x4_t %tmp2, 3
  %tmp7 = add <4 x i16> %tmp3, %tmp4
  %tmp8 = add <4 x i16> %tmp5, %tmp6
  %tmp9 = add <4 x i16> %tmp7, %tmp8
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld4.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <4 x i16> %tmp9
}

define <2 x i32> @vld4lanei32(i32* %A, <2 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <2 x i32>, <2 x i32>* %B
  %tmp2 = call %struct.__neon_int32x2x4_t @llvm.arm.neon.vld4lane.v2i32(i8* %tmp0, <2 x i32> %tmp1, <2 x i32> %tmp1, <2 x i32> %tmp1, <2 x i32> %tmp1, i32 1, i32 8)
  %tmp3 = extractvalue %struct.__neon_int32x2x4_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x2x4_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int32x2x4_t %tmp2, 2
  %tmp6 = extractvalue %struct.__neon_int32x2x4_t %tmp2, 3
  %tmp7 = add <2 x i32> %tmp3, %tmp4
  %tmp8 = add <2 x i32> %tmp5, %tmp6
  %tmp9 = add <2 x i32> %tmp7, %tmp8
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld4.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:64]
  ret <2 x i32> %tmp9
}

define <8 x i16> @vld4laneQi16(i16* %A, <8 x i16>* %B) nounwind {
  %tmp0 = bitcast i16* %A to i8*
  %tmp1 = load <8 x i16>, <8 x i16>* %B
  %tmp2 = call %struct.__neon_int16x8x4_t @llvm.arm.neon.vld4lane.v8i16(i8* %tmp0, <8 x i16> %tmp1, <8 x i16> %tmp1, <8 x i16> %tmp1, <8 x i16> %tmp1, i32 1, i32 16)
  %tmp3 = extractvalue %struct.__neon_int16x8x4_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int16x8x4_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int16x8x4_t %tmp2, 2
  %tmp6 = extractvalue %struct.__neon_int16x8x4_t %tmp2, 3
  %tmp7 = add <8 x i16> %tmp3, %tmp4
  %tmp8 = add <8 x i16> %tmp5, %tmp6
  %tmp9 = add <8 x i16> %tmp7, %tmp8
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld4.16 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0:64]
  ret <8 x i16> %tmp9
}

define <4 x i32> @vld4laneQi32(i32* %A, <4 x i32>* %B) nounwind {
  %tmp0 = bitcast i32* %A to i8*
  %tmp1 = load <4 x i32>, <4 x i32>* %B
  %tmp2 = call %struct.__neon_int32x4x4_t @llvm.arm.neon.vld4lane.v4i32(i8* %tmp0, <4 x i32> %tmp1, <4 x i32> %tmp1, <4 x i32> %tmp1, <4 x i32> %tmp1, i32 2, i32 1)
  %tmp3 = extractvalue %struct.__neon_int32x4x4_t %tmp2, 0
  %tmp4 = extractvalue %struct.__neon_int32x4x4_t %tmp2, 1
  %tmp5 = extractvalue %struct.__neon_int32x4x4_t %tmp2, 2
  %tmp6 = extractvalue %struct.__neon_int32x4x4_t %tmp2, 3
  %tmp7 = add <4 x i32> %tmp3, %tmp4
  %tmp8 = add <4 x i32> %tmp5, %tmp6
  %tmp9 = add <4 x i32> %tmp7, %tmp8
; CHECK:         bic r0, r0, #-1073741824
; CHECK-NEXT:    vld4.32 {{{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}, {{d[0-9]+\[[0-9]\]}}}, [r0]
  ret <4 x i32> %tmp9
}


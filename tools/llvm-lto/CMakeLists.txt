set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  LTO
  MC
  NaClTransforms
  Support
  )

add_llvm_tool(llvm-lto
  llvm-lto.cpp
  )


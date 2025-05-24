// Copyright (C) 2021-2023 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "action/compiler.h"  // JIT编译器接口头文件
#include "common/enums.h"     // 通用枚举定义

#ifdef ZEN_ENABLE_SINGLEPASS_JIT
#include "singlepass/singlepass.h"  // 单遍JIT编译器实现
#endif
#ifdef ZEN_ENABLE_MULTIPASS_JIT
#include "compiler/compiler.h"      // 多遍JIT编译器实现
#endif

namespace zen::action {

void performJITCompile(runtime::Module &Mod) {  // 执行JIT编译的统一入口函数
  switch (Mod.getRuntime()->getConfig().Mode) {  // 根据运行时配置的模式选择编译策略
#ifdef ZEN_ENABLE_SINGLEPASS_JIT
  case common::RunMode::SinglepassMode: {  // 单遍编译模式：快速编译，适合启动时间敏感的场景
    singlepass::JITCompiler::compile(&Mod);  // 调用单遍JIT编译器进行编译
    break;
  }
#endif
#ifdef ZEN_ENABLE_MULTIPASS_JIT
  case common::RunMode::MultipassMode: {  // 多遍编译模式：优化编译，适合长时间运行的场景
    if (Mod.getRuntime()->getConfig().EnableMultipassLazy) {  // 如果启用懒编译模式
      auto *LCompiler = Mod.newLazyJITCompiler();  // 创建懒编译器实例
      LCompiler->precompile();  // 执行预编译（只编译必要的函数）
    } else {  // 如果使用急切编译模式
      COMPILER::EagerJITCompiler ECompiler(&Mod);  // 创建急切编译器实例
      ECompiler.compile();  // 立即编译所有函数
    }
    break;
  }
#endif
  default:  // 默认情况：不进行JIT编译，使用解释器模式
    break;
  }
}

} // namespace zen::action

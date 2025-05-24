// Copyright (C) 2021-2023 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "action/instantiator.h"
#include "common/defines.h"
#include "common/enums.h"
#include "common/type.h"
#include "runtime/instance.h"
#include "utils/math.h"
#include <algorithm>

namespace zen::action {

using namespace common;
using namespace runtime;

// 实例化全局变量
// 初始化模块中的所有全局变量，包括导入的和内部定义的全局变量
// 设置全局变量的偏移量、可变性、类型，并初始化其值
void Instantiator::instantiateGlobals(Instance &Inst) {
  const Module &Mod = *Inst.Mod;  // 获取模块引用
  Inst.NumTotalGlobals = Mod.getNumTotalGlobals();  // 设置全局变量总数

  uint32_t Idx = 0;  // 全局变量索引计数器

  for (uint32_t I = 0; I < Mod.NumImportGlobals; ++I) {  // 遍历所有导入的全局变量
    const auto &Global = Mod.ImportGlobalTable[I];  // 获取导入全局变量定义
    uint32_t Offset = Global.Offset;  // 获取全局变量在内存中的偏移量
    // uint8_t *GlobalPtr = Inst.GlobalVarData + Offset;
    GlobalInstance &GlobalInst = Inst.Globals[Idx];  // 获取全局变量实例
    GlobalInst.Offset = Offset;  // 设置偏移量
    GlobalInst.Mutable = Global.Mutable;  // 设置可变性
    GlobalInst.Type = Global.Type;  // 设置类型
    ++Idx;  // 递增索引
  }

  size_t GlobalVarSize = Inst.getModule()->Layout.GlobalVarSize;  // 获取全局变量数据区大小
  if (GlobalVarSize > 0) {  // 如果有全局变量数据
    std::memset(Inst.GlobalVarData, 0, GlobalVarSize);  // 清零全局变量数据区
  }

  for (uint32_t I = 0; I < Mod.NumInternalGlobals; ++I) {  // 遍历所有内部全局变量
    const auto &Global = Mod.InternalGlobalTable[I];  // 获取内部全局变量定义
    uint32_t Offset = Global.Offset;  // 获取偏移量
    uint32_t Size = getWASMTypeSize(Global.Type);  // 获取类型大小

    GlobalInstance &GlobalInst = Inst.Globals[Idx];  // 获取全局变量实例
    GlobalInst.Offset = Offset;  // 设置偏移量
    GlobalInst.Mutable = Global.Mutable;  // 设置可变性
    GlobalInst.Type = Global.Type;  // 设置类型

    uint8_t *GlobalPtr = Inst.GlobalVarData + Offset;  // 计算全局变量数据指针
    const auto &InitExpr = Global.InitExprVal;  // 获取初始化表达式值
    switch (Global.InitExprKind) {  // 根据初始化表达式类型处理
    case GET_GLOBAL: {  // 从其他全局变量获取初始值
      const auto &FromGlobalInst = Mod.InternalGlobalTable[InitExpr.GlobalIdx];  // 获取源全局变量
      // can move global validity to module validate session;
      // 可以将全局变量有效性检查移到模块验证阶段
      ZEN_ASSERT(Size <= sizeof(FromGlobalInst.InitExprVal));  // 确保大小不超限
      std::memcpy(GlobalPtr, &FromGlobalInst.InitExprVal.I32, Size);  // 复制初始值
      break;
    }
    default:  // 直接使用常量初始值
      ZEN_ASSERT(Size <= sizeof(InitExpr));  // 确保大小不超限
      std::memcpy(GlobalPtr, &InitExpr.I32, Size);  // 复制初始值
      break;
    }
    ++Idx;  // 递增索引
  }
}

// 实例化函数
// 初始化模块中的所有函数，包括导入函数和内部函数
// 设置函数类型、参数、返回值、代码指针等信息，支持JIT编译和调用栈转储
void Instantiator::instantiateFunctions(Instance &Inst) {
#ifdef ZEN_ENABLE_DUMP_CALL_STACK
  auto &HostFuncPtrs = Inst.HostFuncPtrs;  // 获取宿主函数指针列表（用于调用栈转储）
#define INSERT_HOST_FUNC_PTR(FuncIdx, Ptr)                                     \
  HostFuncPtrs.emplace_back(FuncIdx, Ptr);  // 插入宿主函数指针的宏定义
#define SORT_HOST_FUNC_PTRS                                                    \
  if (!HostFuncPtrs.empty()) {                                                 \
    uintptr_t HostFuncBoundEnd =                                               \
        HostFuncPtrs.back().second + MAX_NATIVE_FUNC_SIZE;                     \
    HostFuncPtrs.emplace_back(-1, HostFuncBoundEnd);                           \
  }                                                                            \
  std::sort(HostFuncPtrs.begin(), HostFuncPtrs.end(),                          \
            [](const auto &A, const auto &B) { return A.second < B.second; });  // 排序宿主函数指针的宏定义
#else
#define INSERT_HOST_FUNC_PTR(...)  // 禁用调用栈转储时的空宏
#define SORT_HOST_FUNC_PTRS        // 禁用调用栈转储时的空宏
#endif

  const Module &Mod = *Inst.Mod;  // 获取模块引用
  Inst.NumTotalFunctions = Mod.getNumTotalFunctions();  // 设置函数总数

  uint32_t NumImportFunctions = Mod.getNumImportFunctions();  // 获取导入函数数量
  if (NumImportFunctions > 0) {  // 如果有导入函数
    std::memset(Inst.Functions, 0,
                sizeof(FunctionInstance) * NumImportFunctions);  // 清零导入函数实例数组
  }
  for (uint32_t I = 0; I < Inst.NumTotalFunctions; ++I) {  // 遍历所有函数
    FunctionInstance &FuncInst = Inst.Functions[I];  // 获取函数实例

    bool IsImport = I < NumImportFunctions;  // 判断是否为导入函数
    // Ensure only used when I >= NumImportFunctions
    // 确保只在I >= NumImportFunctions时使用
    uint32_t InternalFuncIdx = I - NumImportFunctions;  // 计算内部函数索引
    uint32_t TypeIdx = IsImport
                           ? Mod.getImportFunction(I).TypeIdx  // 获取导入函数的类型索引
                           : Mod.getInternalFunction(InternalFuncIdx).TypeIdx;  // 获取内部函数的类型索引

    TypeEntry &Type = Mod.TypeTable[TypeIdx];  // 获取函数类型定义
    FuncInst.NumParams = Type.NumParams;  // 设置参数数量
    FuncInst.NumParamCells = Type.NumParamCells;  // 设置参数占用的栈单元数
    FuncInst.NumReturns = Type.NumReturns;  // 设置返回值数量
    FuncInst.NumReturnCells = Type.NumReturnCells;  // 设置返回值占用的栈单元数
    std::memcpy(FuncInst.ReturnTypes, Type.ReturnTypes,
                sizeof(FuncInst.ReturnTypes));  // 复制返回值类型数组
    FuncInst.ParamTypes = Type.ParamTypes;  // 设置参数类型指针
    FuncInst.FuncType = &Type;  // 设置函数类型指针

    if (IsImport) {  // 如果是导入函数（Native函数）
      FuncInst.Kind = FunctionKind::Native;  // 设置为Native函数类型
      FuncInst.CodePtr =
          reinterpret_cast<const uint8_t *>(Mod.ImportFunctionTable[I].FuncPtr);  // 设置函数指针
      INSERT_HOST_FUNC_PTR(I, uintptr_t(FuncInst.CodePtr))  // 插入宿主函数指针（用于调用栈转储）
    } else {  // 如果是内部函数（ByteCode函数）
      FuncInst.Kind = FunctionKind::ByteCode;  // 设置为ByteCode函数类型
      const CodeEntry &Code = Mod.CodeTable[InternalFuncIdx];  // 获取代码条目
      FuncInst.NumLocals = Code.NumLocals;  // 设置局部变量数量
      FuncInst.NumLocalCells = Code.NumLocalCells;  // 设置局部变量占用的栈单元数
      FuncInst.LocalTypes = Code.LocalTypes;  // 设置局部变量类型指针
      FuncInst.LocalOffsets = Code.LocalOffsets;  // 设置局部变量偏移量指针
      FuncInst.MaxStackSize = Code.MaxStackSize;  // 设置最大栈大小
      FuncInst.MaxBlockDepth = Code.MaxBlockDepth;  // 设置最大块嵌套深度
      FuncInst.CodePtr = Code.CodePtr;  // 设置字节码指针
#ifdef ZEN_ENABLE_JIT
      FuncInst.JITCodePtr = Code.JITCodePtr;  // 设置JIT编译后的代码指针
#endif
      FuncInst.CodeSize = Code.CodeSize;  // 设置代码大小
    }

#ifdef ZEN_ENABLE_JIT
    Inst.FuncTypeIdxs[I] = TypeIdx;  // 设置函数类型索引（用于JIT）
    Inst.JITFuncPtrs[I] = reinterpret_cast<uintptr_t>(FuncInst.JITCodePtr);  // 设置JIT函数指针
#endif
  }

  SORT_HOST_FUNC_PTRS  // 排序宿主函数指针（用于调用栈转储）
}

// 实例化表
// 初始化模块中的所有表，包括导入表和内部表
// 设置表的大小限制，分配元素存储空间，并初始化元素段
void Instantiator::instantiateTables(Instance &Inst) {
  const Module &Mod = *Inst.Mod;  // 获取模块引用
  Inst.NumTotalTables = Mod.getNumTotalTables();  // 设置表总数

  uint32_t *TableElemStart = reinterpret_cast<uint32_t *>(
      (uintptr_t)Inst.Tables + Inst.Mod->Layout.TableInstancesSize);  // 计算表元素数据的起始地址

  for (uint32_t I = 0; I < Inst.NumTotalTables; ++I) {  // 遍历所有表
    TableInstance &TableInst = Inst.Tables[I];  // 获取表实例
    if (I < Mod.NumImportTables) {  // 如果是导入表
      const auto &Table = Mod.ImportTableTable[I];  // 获取导入表定义
      TableInst.CurSize = Table.InitSize;  // 设置当前大小
      TableInst.MaxSize = Table.MaxSize;   // 设置最大大小
    } else {  // 如果是内部表
      const auto &Table = Mod.InternalTableTable[I - Mod.NumImportTables];  // 获取内部表定义
      TableInst.CurSize = Table.InitSize;  // 设置当前大小
      TableInst.MaxSize = Table.MaxSize;   // 设置最大大小
    }

    std::memset(TableElemStart, -1, TableInst.CurSize * sizeof(uint32_t));  // 初始化表元素为-1（无效索引）
    TableInst.Elements = TableElemStart;  // 设置表元素指针
  }

  for (uint32_t I = 0; I < Mod.NumElementSegments; ++I) {  // 遍历所有元素段
    const auto &Element = Mod.ElementTable[I];  // 获取元素段定义
    TableInstance &TableInst = Inst.Tables[Element.TableIdx];  // 获取目标表实例
    uint32_t Offset = 0;  // 初始化偏移量
    if (Element.InitExprKind == GET_GLOBAL) {  // 如果偏移量来自全局变量
      uint32_t GlobalIdx = Element.InitExprVal.GlobalIdx;  // 获取全局变量索引
      Offset = Inst.Globals[GlobalIdx].Offset;  // 获取全局变量偏移量
      Offset = *reinterpret_cast<uint32_t *>(Inst.GlobalVarData + Offset);  // 读取全局变量值作为偏移量
    } else {  // 如果偏移量是常量
      Offset = Element.InitExprVal.I32;  // 直接使用常量值
    }

    const uint32_t NumFuncIdxs = Element.NumFuncIdxs;  // 获取函数索引数量
    if (Offset > TableInst.CurSize ||
        (Offset + NumFuncIdxs) > TableInst.CurSize) {  // 检查元素段是否超出表边界
#ifdef ZEN_ENABLE_DWASM
      throw getError(ErrorCode::DWasmModuleFormatInvalid);  // DWASM模式下的错误
#else
      throw getError(ErrorCode::ElementsSegmentDoesNotFit);  // 标准模式下的错误
#endif
    }

    std::memcpy(TableInst.Elements + Offset, Element.FuncIdxs,
                NumFuncIdxs * sizeof(uint32_t));  // 复制函数索引到表中
  }
}

// 检查和更新内存页数
// 验证当前内存页数不超过虚拟机限制，并更新最大页数限制
static void checkAndUpdateMemPages(uint32_t VmMaxMemPages, uint32_t CurMemPages,
                                   uint32_t *MaxMemPages) {
  if (VmMaxMemPages > 0) {  // 如果设置了虚拟机内存页数限制
    if (CurMemPages > VmMaxMemPages) {  // 如果当前页数超过限制
      throw getErrorWithPhase(ErrorCode::MemorySizeTooLarge,
                              ErrorPhase::Instantiation);  // 抛出内存大小过大错误
    }
    *MaxMemPages = std::min(*MaxMemPages, VmMaxMemPages);  // 更新最大页数为较小值
  }
}

// 通过数据段初始化内存
// 将模块中定义的数据段复制到相应的内存实例中
void Instantiator::initMemoryByDataSegments(Instance &Inst) {
  if (Inst.DataSegsInited) {  // 如果数据段已经初始化
    return;  // 直接返回
  }
  const Module *Mod = Inst.Mod;  // 获取模块指针
  for (uint32_t I = 0; I < Mod->NumDataSegments; ++I) {  // 遍历所有数据段
    const auto &DataSeg = Mod->DataTable[I];  // 获取数据段定义
    uint32_t MemIdx = DataSeg.MemIdx;  // 获取目标内存索引
    // should checked if MemIndex is valid in loader
    // 应该在加载器中检查内存索引是否有效
    MemoryInstance &MemInst = Inst.Memories[MemIdx];  // 获取内存实例

    uint32_t Offset = 0;  // 初始化偏移量

    if (DataSeg.InitExprKind == GET_GLOBAL) {  // 如果偏移量来自全局变量
      uint32_t GlobalIdx = DataSeg.InitExprVal.GlobalIdx;  // 获取全局变量索引
      auto *InitExprPtr = Inst.GlobalVarData + Inst.Globals[GlobalIdx].Offset;  // 计算全局变量地址
      Offset = *reinterpret_cast<uint32_t *>(InitExprPtr);  // 读取全局变量值作为偏移量
    } else {  // 如果偏移量是常量
      Offset = DataSeg.InitExprVal.I32;  // 直接使用常量值
    }

    uint32_t DataBoundary;  // 数据边界
    if (Offset > MemInst.MemSize ||
        utils::addOverflow(Offset, DataSeg.Size, DataBoundary) ||
        DataBoundary > MemInst.MemSize) {  // 检查数据段是否超出内存边界
      throw getError(ErrorCode::DataSegmentDoesNotFit);  // 抛出数据段不适合错误
    }

    if (MemInst.MemBase) {  // 如果内存基地址有效
      std::memcpy(MemInst.MemBase + Offset,
                  Mod->getWASMBytecode() + DataSeg.Offset, DataSeg.Size);  // 复制数据段到内存
    }
  }
  Inst.DataSegsInited = true;  // 标记数据段已初始化
}

// 实例化内存
// 初始化模块中的所有内存，包括导入内存和内部内存
// 分配内存空间，设置内存限制，并初始化数据段
void Instantiator::instantiateMemories(Instance &Inst) {
  const Module &Mod = *Inst.Mod;  // 获取模块引用
  Inst.NumTotalMemories = Mod.getNumTotalMemories();  // 设置内存总数
  if (Inst.NumTotalMemories > 1) {  // WebAssembly 1.0只支持单个内存
    throw getErrorWithPhase(ErrorCode::TooManyMemories,
                            ErrorPhase::Instantiation);  // 抛出内存过多错误
  }

  uint32_t VmMaxMemPages = Inst.getRuntime()->getVmMaxMemoryPages();  // 获取虚拟机最大内存页数限制

  for (uint32_t I = 0; I < Inst.NumTotalMemories; ++I) {  // 遍历所有内存
    MemoryInstance &MemInst = Inst.Memories[I];  // 获取内存实例

    if (I < Mod.NumImportMemories) {  // 如果是导入内存
      const auto &ImportMem = Mod.ImportMemoryTable[I];  // 获取导入内存定义
      uint32_t CurMemPages = ImportMem.InitPages;  // 获取初始页数
      uint32_t MaxMemPages = ImportMem.MaxPages;   // 获取最大页数
      checkAndUpdateMemPages(VmMaxMemPages, CurMemPages, &MaxMemPages);  // 检查和更新页数限制
      MemInst.CurPages = CurMemPages;  // 设置当前页数
      MemInst.MaxPages = MaxMemPages;  // 设置最大页数

      MemInst.MemBase = nullptr;  // 导入内存暂不分配实际内存
      MemInst.MemEnd = nullptr;   // 设置内存结束地址为空
      MemInst.MemSize = 0;        // 设置内存大小为0
      MemInst.Kind = WasmMemoryDataType::WM_MEMORY_DATA_TYPE_NO_DATA;  // 设置为无数据类型
    } else {  // 如果是内部内存
      uint32_t InternalMemIdx = I - Mod.NumImportMemories;  // 计算内部内存索引
      const auto &Mem = Mod.InternalMemoryTable[InternalMemIdx];  // 获取内部内存定义
      uint32_t CurMemPages = Mem.InitSize;  // 获取初始页数

      uint32_t MaxMemPages = 0;  // 初始化最大页数
      if (!Mem.MaxSize) {  // 如果未设置最大大小
        MaxMemPages = MemInst.CurPages;  // 使用当前页数作为最大页数
      } else {  // 如果设置了最大大小
        MaxMemPages = Mem.MaxSize;  // 使用设置的最大大小
      }
      checkAndUpdateMemPages(VmMaxMemPages, CurMemPages, &MaxMemPages);  // 检查和更新页数限制
      MemInst.CurPages = CurMemPages;  // 设置当前页数
      MemInst.MaxPages = MaxMemPages;  // 设置最大页数

      if (!MemInst.CurPages) {  // 如果当前页数为0
        MemInst.MemSize = 0;        // 设置内存大小为0
        MemInst.MemBase = nullptr;  // 设置内存基地址为空
        MemInst.MemEnd = nullptr;   // 设置内存结束地址为空
        MemInst.Kind = WasmMemoryDataType::WM_MEMORY_DATA_TYPE_NO_DATA;  // 设置为无数据类型
      }
    }

    uint64_t TotalMemSize =
        MemInst.CurPages * uint64_t(common::DefaultBytesNumPerPage);  // 计算总内存大小（页数×每页字节数）
    ZEN_ASSERT(TotalMemSize <= UINT32_MAX);  // 确保内存大小不超过32位限制

    auto *MemAllocator = Inst.getWasmMemoryAllocator();  // 获取WebAssembly内存分配器
    bool DataSegsInited = false;  // 数据段初始化标志

    WasmMemoryData NewMemDataStruct = MemAllocator->allocInitWasmMemory(
        (uint8_t *)reinterpret_cast<void *>(&MemInst), TotalMemSize, true,
        &DataSegsInited, nullptr, 0);  // 分配并初始化WebAssembly内存
    if (!NewMemDataStruct.MemoryData && TotalMemSize > 0) {  // 如果分配失败且需要内存
      ZEN_ABORT();  // 程序终止
    }
    Inst.DataSegsInited = DataSegsInited;  // 设置数据段初始化状态

    MemInst.MemSize = TotalMemSize;  // 设置内存大小
    MemInst.MemBase = reinterpret_cast<uint8_t *>(NewMemDataStruct.MemoryData);  // 设置内存基地址
    MemInst.Kind = NewMemDataStruct.Type;  // 设置内存数据类型
    MemInst.MemEnd = MemInst.MemBase + TotalMemSize;  // 计算内存结束地址
  }

  initMemoryByDataSegments(Inst);  // 通过数据段初始化内存内容
}

#ifdef ZEN_ENABLE_BUILTIN_WASI
// will move the following to other place soon.
// 将来会将以下代码移到其他地方
// 实例化WASI（WebAssembly System Interface）
// 初始化WASI上下文，设置命令行参数、环境变量和目录权限
void Instantiator::instantiateWasi(Instance &Inst) {
  Runtime *RT = Inst.getRuntime();  // 获取运行时实例

  HostModule *HostMod =
      RT->resolveHostModule(WASM_SYMBOL_wasi_snapshot_preview1);  // 解析WASI宿主模块
  ZEN_ASSERT(HostMod);  // 确保模块存在

  const BuiltinModuleDesc *HostModDesc = HostMod->getModuleDesc();  // 获取模块描述
  ZEN_ASSERT(HostModDesc);  // 确保描述存在

  uint32_t Argc = 0, NumEnvs = 0, NumDirs = 0, ArgvBufSize = 0, EnvBufSize = 0;  // 初始化各种计数器
  auto *ArgvList = RT->getWASIArgs(Argc);  // 获取命令行参数列表
  auto *ArgvBuf = RT->getWASIArgsBuf(ArgvBufSize);  // 获取命令行参数缓冲区
  auto *EnvList = RT->getWASIEnvs(NumEnvs);  // 获取环境变量列表
  auto *EnvBuf = RT->getWASIEnvsBuf(EnvBufSize);  // 获取环境变量缓冲区
  auto *DirList = RT->getWASIDirs(NumDirs);  // 获取目录权限列表

  void *WASICtx = HostModDesc->_init_ctx_func(
      HostMod->getVNMIEnv(), DirList, NumDirs, EnvList, NumEnvs, EnvBuf,
      EnvBufSize, const_cast<char **>(ArgvList), Argc, ArgvBuf, ArgvBufSize);  // 初始化WASI上下文

  // temporally store wasi ctx into instance, will move to wni later.
  // 临时将WASI上下文存储到实例中，稍后会移到WNI
  Inst.WASICtx = (host::WASIContext *)WASICtx;  // 设置WASI上下文
}
#endif

// 实例化WebAssembly模块
// 按顺序初始化全局变量、函数、表、内存，可选地初始化WASI，最后执行启动函数
void Instantiator::instantiate(Instance &Inst) {
  const Module &Mod = *Inst.Mod;  // 获取模块引用

  instantiateGlobals(Inst);  // 实例化全局变量

  instantiateFunctions(Inst);  // 实例化函数

  instantiateTables(Inst);  // 实例化表

  instantiateMemories(Inst);  // 实例化内存

#ifdef ZEN_ENABLE_BUILTIN_WASI
  if (!Inst.getRuntime()->getConfig().DisableWASI) {  // 如果启用了WASI且未禁用
    instantiateWasi(Inst);  // 实例化WASI
  }
#endif

  uint32_t StartFuncIdx = Mod.getStartFuncIdx();  // 获取启动函数索引
  if (StartFuncIdx != -1u) {  // 如果存在启动函数
    Runtime *RT = Inst.getRuntime();  // 获取运行时实例
    std::vector<common::TypedValue> Results;  // 创建结果向量
    if (!RT->callWasmFunction(Inst, StartFuncIdx, {}, Results)) {  // 调用启动函数
      throw common::Error(Inst.getError());  // 如果调用失败则抛出错误
    }
  }
}

} // namespace zen::action

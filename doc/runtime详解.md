# DTVM运行时系统（runtime.h/cpp）详解

`runtime.h`和`runtime.cpp`文件实现了DTVM项目的核心运行时系统，负责管理WebAssembly模块的加载、编译和执行。这个系统是DTVM的核心组件，为整个虚拟机提供了基础设施和执行环境。

## 1. Runtime类概述

`Runtime`类是DTVM运行时系统的核心，它管理着WebAssembly模块的整个生命周期，从加载到执行。

```cpp
class Runtime final {
  // 类型别名定义
  using MemPool = common::SysMemPool;
  using ConstStringPool = common::ConstStringPool;
  using Error = common::Error;
  using ErrorCode = common::ErrorCode;
  using TypedValue = common::TypedValue;
  using RunMode = common::RunMode;

  // 成员变量
  common::Mutex Mtx;                                          // 互斥锁，用于线程安全
  MemPool MPool;                                              // 内存池，用于内存管理
  ConstStringPool SymbolPool;                                 // 符号池，用于字符串管理
  std::unordered_map<WASMSymbol, HostModuleUniquePtr> HostModulePool;  // 主机模块池
  std::unordered_map<WASMSymbol, ModuleUniquePtr> ModulePool;          // WebAssembly模块池
  std::unordered_map<Isolation *, IsolationUniquePtr> Isolations;      // 隔离环境池
  uint32_t VMMaxMemPages = 0;                                 // 最大内存页数
  RuntimeConfig Config;                                       // 运行时配置
  utils::Statistics Stats;                                    // 统计信息

  // WASI相关成员变量
#ifdef ZEN_ENABLE_BUILTIN_WASI
  char *_argv_buf = nullptr;                                  // 命令行参数缓冲区
  uint32_t _argv_buf_size = 0;                                // 命令行参数缓冲区大小
  char **_argv_list = nullptr;                                // 命令行参数列表
  uint32_t _argc = 0;                                         // 命令行参数数量
  char *_env_buf = nullptr;                                   // 环境变量缓冲区
  uint32_t _env_buf_size = 0;                                 // 环境变量缓冲区大小
  char **_env_list = nullptr;                                 // 环境变量列表
  uint32_t _env_count = 0;                                    // 环境变量数量
  char *_dirs_buf = nullptr;                                  // 目录缓冲区
  char **_dirs_list = nullptr;                                // 目录列表
  uint32_t _dirs_count = 0;                                   // 目录数量
#endif
};
```

## 2. 运行时创建与初始化

### 2.1 创建运行时

`Runtime`类提供了静态工厂方法`newRuntime`来创建运行时实例：

```cpp
static std::unique_ptr<Runtime>
newRuntime(RuntimeConfig Config = {}) noexcept {
  if (!Config.validate()) {
    ZEN_LOG_ERROR("runtime config validation failed");
    return nullptr;
  }

  std::unique_ptr<Runtime> RT(new Runtime(Config));

  if (!RT->initRuntime()) {
    ZEN_LOG_ERROR("initialize runtime failed");
    return nullptr;
  }

#ifdef ZEN_ENABLE_DWASM
  RT->setVmMaxMemoryPages(DWASM_DEFAULT_MAX_VM_LINEAR_MEMORY_PAGES);
#endif // ZEN_ENABLE_DWASM

  return RT;
}
```

这个方法首先验证配置的有效性，然后创建`Runtime`实例并初始化它。如果初始化失败，则返回`nullptr`。

### 2.2 运行时初始化

运行时初始化主要是初始化符号池：

```cpp
bool initRuntime() { return SymbolPool.initPool(); }
```

### 2.3 运行时清理

当运行时被销毁时，会调用`cleanRuntime`方法清理资源：

```cpp
void Runtime::cleanRuntime() {
  Isolations.clear();
  HostModulePool.clear();
  ModulePool.clear();
  SymbolPool.destroyPool();

#ifdef ZEN_ENABLE_BUILTIN_WASI
  if (_argv_buf)
    deallocate(_argv_buf);
  if (_argv_list)
    deallocate(_argv_list);
  if (_env_buf)
    deallocate(_env_buf);
  if (_env_list)
    deallocate(_env_list);
  if (_dirs_buf)
    deallocate(_dirs_buf);
  if (_dirs_list)
    deallocate(_dirs_list);
#endif
}
```

这个方法清理了所有隔离环境、主机模块、WebAssembly模块和符号池，以及WASI相关的资源。

## 3. 模块管理

### 3.1 加载WebAssembly模块

`Runtime`类提供了多种方法来加载WebAssembly模块：

#### 从文件加载模块

```cpp
MayBe<Module *> Runtime::loadModule(const std::string &Filename,
                                    const std::string &EntryHint) noexcept {
  if (Filename.empty()) {
    return getError(ErrorCode::InvalidFilePath);
  }

  WASMSymbol Name = newSymbol(Filename.c_str(), Filename.size());
  if (auto It = ModulePool.find(Name); It != ModulePool.end()) {
    return It->second.get();  // 如果模块已加载，直接返回
  }

  try {
    auto Code = CodeHolder::newFileCodeHolder(*this, Filename);
    return loadModule(Name, std::move(Code), EntryHint);
  } catch (const Error &Err) {
    Stats.clearAllTimers();
    freeSymbol(Name);
    return Err;
  }
}
```

#### 从内存加载模块

```cpp
MayBe<Module *> Runtime::loadModule(const std::string &ModName,
                                    const void *Data, size_t Size,
                                    const std::string &EntryHint) noexcept {
  if (ModName.empty() || !Data || !Size) {
    return getError(ErrorCode::InvalidRawData);
  }

  WASMSymbol Name = newSymbol(ModName.c_str(), ModName.size());
  if (auto It = ModulePool.find(Name); It != ModulePool.end()) {
    return It->second.get();  // 如果模块已加载，直接返回
  }

  try {
    auto Code = CodeHolder::newRawDataCodeHolder(*this, Data, Size);
    return loadModule(Name, std::move(Code), EntryHint);
  } catch (const Error &Err) {
    Stats.clearAllTimers();
    freeSymbol(Name);
    return Err;
  }
}
```

#### 内部加载模块实现

```cpp
Module *Runtime::loadModule(WASMSymbol Name, CodeHolderUniquePtr CodeHolder,
                            const std::string &EntryHint) {
  ZEN_ASSERT(Name);
  ZEN_ASSERT(CodeHolder);

  ModuleUniquePtr Mod =
      Module::newModule(*this, std::move(CodeHolder), EntryHint);
  // 所有错误都会作为异常抛出，所以这里的返回值一定是有效的
  ZEN_ASSERT(Mod);
  auto *ModulePtr = Mod.get();
  ModulePtr->setName(Name);

  // 忽略返回值，因为名称是唯一的（在上面已经检查过）
  auto EmplaceRet =
      ModulePool.emplace(Name, std::forward<ModuleUniquePtr>(Mod));
  if (EmplaceRet.second) {
    return EmplaceRet.first->second.get();
  }

  return ModulePtr;
}
```

这个方法是内部实现，它创建一个新的`Module`对象，设置其名称，并将其添加到模块池中。

### 3.2 卸载WebAssembly模块

```cpp
bool Runtime::unloadModule(const Module *Mod) noexcept {
  WASMSymbol Name = Mod->getName();
  return ModulePool.erase(Name) != 0;
}
```

这个方法从模块池中移除指定的模块。

### 3.3 加载主机模块

```cpp
HostModule *Runtime::loadHostModule(BuiltinModuleDesc &HostModDesc) noexcept {
  const char *ModName = HostModDesc._name;
  WASMSymbol Name = newSymbol(ModName, std::strlen(ModName));
  if (auto It = HostModulePool.find(Name); It != HostModulePool.end()) {
    return It->second.get();  // 如果模块已加载，直接返回
  }

  HostModuleUniquePtr HostMod = HostModule::newModule(*this, &HostModDesc);
  if (!HostMod) {
    freeSymbol(Name);
    return nullptr;
  }

  auto EmplaceRet =
      HostModulePool.emplace(Name, std::forward<HostModuleUniquePtr>(HostMod));
  if (!EmplaceRet.second) {
    freeSymbol(Name);
    return nullptr;
  }

  return EmplaceRet.first->second.get();
}
```

这个方法加载一个主机模块，主机模块提供了WebAssembly与主机环境交互的接口。

## 4. 隔离环境管理

### 4.1 创建隔离环境

```cpp
Isolation *Runtime::createManagedIsolation() noexcept {
  IsolationUniquePtr Iso = createUnmanagedIsolation();
  if (!Iso) {
    return nullptr;
  }

  Isolation *RawIso = Iso.get();
  common::LockGuard<common::Mutex> Lock(Mtx);
  auto EmplaceRet =
      Isolations.emplace(RawIso, std::forward<IsolationUniquePtr>(Iso));
  if (!EmplaceRet.second) {
    return nullptr;
  }

  return EmplaceRet.first->second.get();
}
```

这个方法创建一个由运行时管理的隔离环境。隔离环境提供了WebAssembly执行的隔离，确保不同实例之间相互隔离。

### 4.2 删除隔离环境

```cpp
bool Runtime::deleteManagedIsolation(Isolation *Iso) noexcept {
  ZEN_ASSERT(Iso);
  common::LockGuard<common::Mutex> Lock(Mtx);
  return Isolations.erase(Iso) != 0;
}
```

这个方法从隔离环境池中移除指定的隔离环境。

### 4.3 创建非托管隔离环境

```cpp
IsolationUniquePtr Runtime::createUnmanagedIsolation() noexcept {
  return Isolation::newIsolation(*this);
}
```

这个方法创建一个非托管的隔离环境，由调用者负责管理其生命周期。

## 5. WebAssembly函数调用

### 5.1 调用WebAssembly主函数

```cpp
bool Runtime::callWasmMain(Instance &Inst, std::vector<TypedValue> &Results) {
  const Module *Mod = Inst.getModule();
  uint32_t FuncIdx;
  if (Mod->getExportFunc("_start", FuncIdx)) {
    TypeEntry *Type = Mod->getFunctionType(FuncIdx);
    if (!checkWASIStartFuncType(Type)) {
      Inst.setError(getErrorWithExtraMessage(ErrorCode::UnexpectedFuncType,
                                             "of wasi '_start' function"));
      ZEN_LOG_ERROR("invalid wasi _start function type");
      return false;
    }
    return callWasmFunction(Inst, FuncIdx, {}, Results);
  }

  // 尝试调用main函数...
}
```

这个方法尝试调用WebAssembly模块的主函数，首先尝试调用`_start`函数（WASI入口点），如果不存在，则尝试调用`main`函数。

### 5.2 调用WebAssembly函数（按名称）

```cpp
bool Runtime::callWasmFunction(Instance &Inst, const std::string &FuncName,
                               const std::vector<std::string> &Args,
                               std::vector<TypedValue> &Results) {
  const Module *Mod = Inst.getModule();

  uint32_t FuncIdx;

  if (!Mod->getExportFunc(FuncName, FuncIdx)) {
    Inst.setError(getErrorWithExtraMessage(ErrorCode::CannotFindFunction,
                                           '"' + FuncName + '"'));
    ZEN_LOG_ERROR("cannot find function '%s'", FuncName.c_str());
    return false;
  }

  FunctionInstance &Func = *Inst.getFunctionInst(FuncIdx);
  uint16_t NumParams = Func.NumParams;
  if (Args.size() != NumParams) {
    Inst.setError(common::getError(ErrorCode::UnexpectedNumArgs));
    ZEN_LOG_ERROR("unexpected number of function arguments");
    return false;
  }

  // 转换参数并调用函数...
}
```

这个方法根据函数名称调用WebAssembly函数，它首先查找函数，然后检查参数数量，最后转换参数并调用函数。

### 5.3 调用WebAssembly函数（按索引）

```cpp
bool Runtime::callWasmFunction(Instance &Inst, uint32_t FuncIdx,
                               const std::vector<TypedValue> &Args,
                               std::vector<TypedValue> &Results) {
  // 检查DWASM限制...

  FunctionInstance *Func = Inst.getFunctionInst(FuncIdx);
  if (!Func) {
    Inst.setError(common::getError(ErrorCode::CannotFindFunction));
    ZEN_LOG_ERROR("cannot find function at index %u", FuncIdx);
    return false;
  }

  // 检查参数数量和类型...

  // 准备接收返回值的槽位
  ZEN_ASSERT(Results.empty());
  uint32_t NumReturns = Func->NumReturns;
  Results.resize(NumReturns);
  for (uint32_t I = 0; I < NumReturns; ++I) {
    Results[I].Type = Func->ReturnTypes[I];
  }

  auto Timer = Stats.startRecord(utils::StatisticPhase::Execution);

  Inst.protectMemory();

#ifdef ZEN_ENABLE_VIRTUAL_STACK
  VirtualStackInfo StackInfo(&Inst, FuncIdx, &Args, &Results);
  StackInfo.runInVirtualStack(&callWasmFuncFromVirtualStack);
#else
  callWasmFunctionOnPhysStack(Inst, FuncIdx, Args, Results);
#endif // !ZEN_ENABLE_VIRTUAL_STACK

  Stats.stopRecord(Timer);

  // 检查执行结果...

  return true;
}
```

这个方法根据函数索引调用WebAssembly函数，它首先检查函数是否存在，然后检查参数，准备返回值槽位，最后执行函数并检查结果。

### 5.4 在物理栈上调用WebAssembly函数

```cpp
void Runtime::callWasmFunctionOnPhysStack(
    Instance &Inst, uint32_t FuncIdx, const std::vector<TypedValue> &Args,
    std::vector<common::TypedValue> &Results) noexcept {
  if (getConfig().Mode == RunMode::InterpMode) {
    callWasmFunctionInInterpMode(Inst, FuncIdx, Args, Results);
  } else {
#ifdef ZEN_ENABLE_JIT
    callWasmFunctionInJITMode(Inst, FuncIdx, Args, Results);
#else
    ZEN_UNREACHABLE();
#endif
  }
}
```

这个方法根据运行模式选择在解释器模式或JIT模式下调用WebAssembly函数。

### 5.5 在解释器模式下调用WebAssembly函数

```cpp
void Runtime::callWasmFunctionInInterpMode(Instance &Inst, uint32_t FuncIdx,
                                           const std::vector<TypedValue> &Args,
                                           std::vector<TypedValue> &Results) {
  using namespace action;
  // 创建解释器栈，大小为PresetReservedStackSize
  RuntimeObjectUniquePtr<InterpStack> Stack =
      InterpStack::newInterpStack(*this, PresetReservedStackSize);
  // 创建解释器执行上下文
  InterpreterExecContext Context(&Inst, Stack.get());
  uint8_t *Bottom = Stack->top();

  // 将参数压入栈
  for (const TypedValue &Arg : Args) {
    const UntypedValue &Val = Arg.Value;
    switch (Arg.Type) {
    case WASMType::I32: {
      Stack->push<int32_t>(Val.I32);
      break;
    }
    case WASMType::I64: {
      Stack->push<int64_t>(Val.I64);
      break;
    }
    case WASMType::F32: {
      Stack->push<float>(Val.F32);
      break;
    }
    case WASMType::F64: {
      Stack->push<double>(Val.F64);
      break;
    }
    default:
      ZEN_ASSERT_TODO();
    }
  }

  // 创建解释器并获取函数实例
  BaseInterpreter Interpreter(Context);
  FunctionInstance *Func = Inst.getFunctionInst(FuncIdx);
  // 分配函数帧，包含局部变量、控制栈和值栈
  InterpFrame *Frame = Context.allocFrame(Func, (uint32_t *)Bottom);
  ZEN_ASSERT(Frame != nullptr);

  // 开始CPU跟踪（如果启用）
  Inst.getRuntime()->startCPUTracing();
  try {
    // 执行解释器
    Interpreter.interpret();
  } catch (const Error &Err) {
    // 如果发生错误，结束CPU跟踪并设置错误
    Inst.getRuntime()->endCPUTracing();
    Inst.setError(Err);
    return;
  }

  // 从栈中获取返回值
  Inst.getRuntime()->endCPUTracing();
  uint32_t NumReturns = Func->NumReturns;
  if (NumReturns > 0) {
    uint32_t *ReturnPtr = (uint32_t *)Stack->Bottom;
    for (uint32_t I = 0; I < NumReturns; ++I) {
      WASMType Type = Func->ReturnTypes[I];
      switch (Type) {
      case WASMType::I32: {
        Results[I].Value.I32 = *(int32_t *)ReturnPtr;
        ReturnPtr += 1;
        break;
      }
      case WASMType::I64: {
        Results[I].Value.I64 = *(int64_t *)ReturnPtr;
        ReturnPtr += 2;
        break;
      }
      case WASMType::F32: {
        Results[I].Value.F32 = *(float *)ReturnPtr;
        ReturnPtr += 1;
        break;
      }
      case WASMType::F64: {
        Results[I].Value.F64 = *(double *)ReturnPtr;
        ReturnPtr += 2;
        break;
      }
      default:
        ZEN_ASSERT_TODO();
      }
    }
  }
}
```

这个方法在解释器模式下调用WebAssembly函数，它创建一个解释器栈和执行上下文，将参数压入栈，然后执行函数，最后从栈中获取返回值。

#### 解释器内部实现

`BaseInterpreter::interpret()`方法的实际实现在`BaseInterpreterImpl::interpret()`中，这是一个基于循环的指令解释器，它按照以下步骤执行：

```cpp
void BaseInterpreterImpl::interpret() {
  // 获取当前函数帧
  InterpFrame *Frame = Context.getCurFrame();
  // 获取指令指针和函数结束位置
  const uint8_t *Ip = Frame->Ip;
  const uint8_t *IpEnd = Ip + Frame->FuncInst->CodeSize;

  // 获取值栈指针和控制栈指针
  uint32_t *ValStackPtr = Frame->ValueStackPtr;
  BlockInfo *ControlStackPtr = Frame->CtrlStackPtr;

  // 获取局部变量指针和函数实例
  uint32_t *LocalPtr = (uint32_t *)Frame->LocalPtr;
  FunctionInstance *FuncInst = Frame->FuncInst;

  // 指令循环
  while (Ip < IpEnd) {
    // 获取当前指令
    uint8_t Opcode = *Ip++;

    // 根据指令类型执行相应操作
    switch (Opcode) {
      case UNREACHABLE: {
        throw getError(ErrorCode::Unreachable);
      }
      case NOP: {
        break;
      }
      case BLOCK: {
        // 处理块指令
        uint32_t CellNum = getWASMTypeCellNumFromOpcode(*Ip++);
        findBlockAddr(Ip, IpEnd, ElseAddr, EndAddr);
        Frame->blockPush(ControlStackPtr, EndAddr, ValStackPtr, CellNum, LABEL_BLOCK);
        break;
      }
      case LOOP: {
        // 处理循环指令
        uint32_t CellNum = getWASMTypeCellNumFromOpcode(*Ip++);
        Frame->blockPush(ControlStackPtr, Ip, ValStackPtr, CellNum, LABEL_LOOP);
        break;
      }
      case CALL: {
        // 处理函数调用指令
        uint32_t FuncIdx;
        Ip = readSafeLEBNumber(Ip, FuncIdx);
        FunctionInstance *Callee = Context.getInstance()->getFunctionInst(FuncIdx);
        callFuncInst(Callee, Context, Ip, IpEnd, Frame, ValStackPtr,
                    ControlStackPtr, LocalPtr, FuncInst);
        break;
      }
      case I32_ADD: {
        // 处理i32.add指令
        int32_t Rhs = Frame->valuePop<int32_t>(ValStackPtr);
        int32_t Lhs = Frame->valuePop<int32_t>(ValStackPtr);
        Frame->valuePush<int32_t>(ValStackPtr, Lhs + Rhs);
        break;
      }
      // ... 其他指令处理 ...

      case END: {
        // 处理块结束或函数返回
        if (ControlStackPtr > Frame->CtrlBasePtr + 1) {
          // 块结束
          Frame->blockPop(ControlStackPtr);
        } else {
          // 函数返回
          Context.freeFrame(FuncInst, Frame);
          InterpFrame *PrevFrame = Frame->PrevFrame;
          ValStackPtr -= (FuncInst->NumReturnCells);
          // 复制返回值到上一层帧的值栈
          std::memcpy(LocalPtr, ValStackPtr, FuncInst->NumReturnCells << 2);
          Frame = PrevFrame;
          Context.setCurFrame(Frame);

          if (Frame == nullptr) {
            break; // 执行完成
          }

          // 更新帧信息
          updateFrame(Ip, IpEnd, Frame, ValStackPtr, ControlStackPtr,
                      LocalPtr, FuncInst, true);
        }
        break;
      }
      default: {
        ZEN_LOG_ERROR("munimplemented opcode: 0x%x", Opcode);
        ZEN_ASSERT_TODO();
      }
    }
  }
}
```

解释器的工作原理是逐个解析和执行 WebAssembly 字节码指令，主要组件包括：

1. **函数帧（InterpFrame）**：包含函数执行的上下文，如局部变量、值栈和控制栈

2. **值栈（Value Stack）**：用于存储指令执行的中间值

3. **控制栈（Control Stack）**：用于管理块结构（block、loop、if）和函数调用

4. **指令指针（Ip）**：指向当前正在执行的指令

解释器支持所有WebAssembly标准指令，包括控制流指令（if、block、loop、br等）、函数调用指令（call、call_indirect）、内存操作指令（load、store）和算术指令（add、sub、mul等）。

### 5.6 在JIT模式下调用WebAssembly函数

```cpp
#ifdef ZEN_ENABLE_JIT
void Runtime::callWasmFunctionInJITMode(Instance &Inst, uint32_t FuncIdx,
                                        const std::vector<TypedValue> &Args,
                                        std::vector<TypedValue> &Results) {
  // 获取函数实例
  FunctionInstance *Func = Inst.getFunctionInst(FuncIdx);
  // 设置JIT栈大小
  Inst.setJITStackSize(PresetReservedStackSize);
  // 判断是否为导入函数
  bool IsImport = FuncIdx < Inst.getModule()->getNumImportFunctions();
  // 获取函数指针，导入函数使用CodePtr，内部函数使用JITCodePtr
  auto FuncPtr =
      GenericFunctionPointer(IsImport ? Func->CodePtr : Func->JITCodePtr);

#ifdef ZEN_ENABLE_CPU_EXCEPTION
  // 设置异常处理
  jmp_buf JmpBuf;
  common::traphandler::CallThreadState TLS(&Inst, &JmpBuf,
                                         __builtin_frame_address(0), nullptr);

  // 使用setjmp/longjmp来捕获CPU异常
  int JmpSignum = ::setjmp(JmpBuf);
  if (JmpSignum == 0) {
    TLS.restartHandler();
#endif // ZEN_ENABLE_CPU_EXCEPTION

  // 调用编译后的本地代码
  entrypoint::callNativeGeneral(&Inst, FuncPtr, Args, Results,
                                this->getMemAllocator());

#ifdef ZEN_ENABLE_CPU_EXCEPTION
  } else {
    // 处理CPU异常
    Inst.setError(common::getError(ErrorCode::CPUException));
  }
#endif // ZEN_ENABLE_CPU_EXCEPTION
}
#endif // ZEN_ENABLE_JIT
```

#### JIT编译器实现

DTVM的JIT编译器将WebAssembly字节码编译为本地机器码，主要分为两种模式：

1. **单通道编译器（Singlepass）**：一次性将WebAssembly编译为本地代码，编译速度快，生成代码质量适中

2. **多通道编译器（Multipass）**：多阶段优化编译，生成高质量的优化代码

多通道JIT编译器的编译过程如下：

```cpp
void EagerJITCompiler::compile() {
  // 开始记录编译时间
  auto Timer = Stats.startRecord(zen::utils::StatisticPhase::JITCompilation);

  // 创建主上下文
  WasmFrontendContext MainContext(*WasmMod);
  auto &MainMemPool = MainContext.ThreadMemPool;

  // 创建MIR模块
  MModule Mod(MainContext);
  // 构建所有MIR函数类型
  buildAllMIRFuncTypes(MainContext, Mod, *WasmMod);
  // 设置代码内存池
  MainContext.CodeMPool = &WasmMod->getJITCodeMemPool();

  // 获取导入函数数量和内部函数数量
  const uint32_t NumImportFunctions = WasmMod->getNumImportFunctions();
  ZEN_ASSERT(NumInternalFunctions > 0);

  // 判断是否使用多线程编译
  if (Config.DisableMultipassMultithread) {
    // 单线程编译
    for (uint32_t I = 0; I < NumInternalFunctions; ++I) {
      compileWasmToMC(MainContext, Mod, I, Config.DisableMultipassGreedyRA);
    }
    // 生成目标代码
    emitObjectBuffer(&MainContext);
    // 设置函数的JIT代码指针
    for (const auto &[FuncIdx, FuncOffset] : MainContext.FuncOffsetMap) {
      uint32_t RealFuncIdx = NumImportFunctions + FuncIdx;
      CodeEntry *CE = WasmMod->getCodeEntry(RealFuncIdx);
      ZEN_ASSERT(CE);
      CE->JITCodePtr = MainContext.CodePtr + FuncOffset;
    }
  } else {
    // 多线程编译
    // 创建线程池
    common::ThreadPool<WasmFrontendContext> ThreadPool(
        std::min(Config.NumMultipassThreads, NumInternalFunctions));
    uint32_t NumThreads = ThreadPool.getThreadCount();
    ZEN_LOG_DEBUG("using %u threads for multipass JIT compilation", NumThreads);

    // 创建辅助上下文
    std::vector<WasmFrontendContext> AuxContexts(NumThreads - 1, MainContext);
    std::vector<WasmFrontendContext *> Contexts;
    Contexts.reserve(NumThreads);
    Contexts.push_back(&MainContext);
    for (auto &Ctx : AuxContexts) {
      Contexts.push_back(&Ctx);
    }

    // 设置线程上下文
    auto emitObjectBuffer = [&](WasmFrontendContext *Ctx) {
      emitObjectBuffer(Ctx);
    };
    ThreadPool.setThreadContext(0, &MainContext, emitObjectBuffer);
    for (uint32_t I = 0; I < NumThreads - 1; ++I) {
      ThreadPool.setThreadContext(I + 1, &AuxContexts[I], emitObjectBuffer);
    }

    // 按函数大小排序，优先编译大函数
    std::vector<std::pair<uint32_t, uint32_t>> FuncIdxAndSizes;
    FuncIdxAndSizes.reserve(NumInternalFunctions);
    for (uint32_t I = 0; I < NumInternalFunctions; ++I) {
      uint32_t RealFuncIdx = NumImportFunctions + I;
      CodeEntry *CE = WasmMod->getCodeEntry(RealFuncIdx);
      ZEN_ASSERT(CE);
      FuncIdxAndSizes.emplace_back(I, CE->CodeSize);
    }
    std::sort(FuncIdxAndSizes.begin(), FuncIdxAndSizes.end(),
              [](const auto &LHS, const auto &RHS) {
                return LHS.second > RHS.second;
              });

    // 提交编译任务
    for (const auto &[FuncIdx, FuncSize] : FuncIdxAndSizes) {
      ThreadPool.pushTask([&, FuncIdx = FuncIdx](WasmFrontendContext *Ctx) {
        compileWasmToMC(*Ctx, Mod, FuncIdx, Config.DisableMultipassGreedyRA);
      });
    }

    // 等待所有任务完成
    ThreadPool.setNoNewTask();
    ThreadPool.waitForTasks();

    // 设置函数的JIT代码指针
    for (WasmFrontendContext *Ctx : Contexts) {
      for (const auto &[FuncIdx, FuncSymOffset] : Ctx->FuncOffsetMap) {
        uint32_t RealFuncIdx = NumImportFunctions + FuncIdx;
        CodeEntry *CE = WasmMod->getCodeEntry(RealFuncIdx);
        ZEN_ASSERT(CE);
        CE->JITCodePtr = Ctx->CodePtr + FuncSymOffset;
      }
    }
  }

  // 设置模块的JIT代码
  WasmMod->setJITCode(JITCode);
  Stats.stopRecord(Timer);
}
```

#### 本地代码调用机制

`entrypoint::callNativeGeneral`函数负责调用JIT编译生成的本地代码：

```cpp
void callNativeGeneral(Instance *Instance, GenericFunctionPointer FuncPtr,
                       const std::vector<TypedValue> &Args,
                       std::vector<TypedValue> &Results, SysMemPool *MPool,
                       bool SkipInstanceProcessing) {
  // 分配参数缓冲区
  uint64_t ArgvBuf[32] = {0};
  uint64_t *ArgvNative = ArgvBuf;

  // 计算参数数量
  uint64_t ArgcNative = 1 + MaxFloatRegs * 2 + uint32_t(Args.size()) * 2;
  if (ArgcNative > sizeof(ArgvBuf) / sizeof(uint64_t)) {
    ArgvNative =
        static_cast<uint64_t *>(MPool->allocate(sizeof(uint64_t) * ArgcNative));
    ZEN_ASSERT(ArgvNative);
  }

  // 记录已使用的寄存器数量
  uint32_t NumIntArgs = 0;   // 已使用的整数寄存器数量
  uint32_t NumFpArgs = 0;    // 已使用的浮点寄存器数量
  uint32_t NumStackArgs = 0; // 已使用的栈单元数量

  // 设置寄存器和栈的指针
  V128 *Fps = (V128 *)ArgvNative;                    // 存储浮点寄存器
  uint64_t *Ints = (uint64_t *)(Fps + MaxFloatRegs); // 存储整数寄存器
  uint64_t *Stacks = Ints + MaxIntRegs; // 存储额外的栈参数

  // 将实例指针作为第一个参数
  if (Instance) {
    Ints[NumIntArgs++] = (uint64_t)(uintptr_t)Instance;
  } else {
    SkipInstanceProcessing = true;
  }

  // 处理函数参数
  for (size_t I = 0; I < Args.size(); ++I) {
    const auto &Value = Args[I].Value;
    switch (Args[I].Type) {
    case WASMType::I32: {
      // 处理I32类型参数
      if (NumIntArgs < MaxIntRegs) {
        std::memcpy(Ints + NumIntArgs, &Value.I32, sizeof(int32_t));
        NumIntArgs++;
      } else {
        std::memcpy(Stacks + NumStackArgs, &Value.I32, sizeof(int32_t));
        NumStackArgs++;
      }
      break;
    }
    // ... 其他类型参数处理 ...
    }
  }

  // 开始CPU跟踪
  if (Instance) {
    Instance->getRuntime()->startCPUTracing();
  }

  // 根据返回值类型调用不同的本地函数
  if (Results.empty()) {
    callNative_Void(FuncPtr, ArgvNative, NumStackArgs, SkipInstanceProcessing);
  } else {
    UntypedValue &Value = Results[0].Value;
    switch (Results[0].Type) {
    case WASMType::I32:
      Value.I32 = callNative_Int32(FuncPtr, ArgvNative, NumStackArgs,
                                   SkipInstanceProcessing);
      break;
    // ... 其他返回类型处理 ...
    }
  }

  // 结束CPU跟踪
  if (Instance) {
    Instance->getRuntime()->endCPUTracing();
  }

  // 释放分配的内存
  if (ArgvNative != ArgvBuf) {
    MPool->deallocate(ArgvNative);
  }
}
```

实际的本地代码调用是通过汇编实现的，如`callNative_Int32`等函数，这些函数根据目标平台的调用约定（x86-64或ARM64）设置寄存器和栈参数，然后调用JIT编译生成的本地代码。

这个方法在JIT模式下调用WebAssembly函数，它获取函数的JIT编译后的代码指针，然后通过`callNativeGeneral`函数调用它。

## 6. WASI支持

`Runtime`类提供了WASI（WebAssembly系统接口）支持，允许WebAssembly模块访问系统资源：

### 6.1 设置WASI参数

```cpp
void setWASIArgs(const std::string &wasm_name,
                 const std::vector<std::string> &args) {
  if (_argv_list || _argv_buf) {
    return;
  }

  std::vector<std::string> wasi_args = std::move(args);
  wasi_args.insert(wasi_args.begin(), wasm_name);

  // 分配和初始化参数缓冲区和列表...
}
```

### 6.2 设置WASI环境变量

```cpp
void setWASIEnvs(const std::vector<std::string> &envs) {
  if (_env_buf || _env_list) {
    return;
  }

  // 分配和初始化环境变量缓冲区和列表...
}
```

### 6.3 设置WASI目录

```cpp
void setWASIDirs(const std::vector<std::string> &dirs) {
  if (_dirs_buf || _dirs_list) {
    return;
  }

  // 分配和初始化目录缓冲区和列表...
}
```

## 7. 内存管理

`Runtime`类提供了内存管理功能，使用内存池来分配和释放内存：

```cpp
void *allocate(size_t Size, size_t Align = 0) {
  return MPool.allocate(Size, Align);
}

void *allocateZeros(size_t Size, size_t Align = 0) {
  return MPool.allocateZeros(Size, Align);
}

void *reallocate(void *Ptr, size_t OldSize, size_t NewSize) {
  return MPool.reallocate(Ptr, OldSize, NewSize);
}

void deallocate(void *Ptr) { MPool.deallocate(Ptr); }
```

## 8. 符号管理

`Runtime`类提供了符号管理功能，使用符号池来管理字符串：

```cpp
WASMSymbol newSymbol(const char *Str, size_t Len) {
  return SymbolPool.newSymbol(Str, Len);
}

WASMSymbol probeSymbol(const char *Str, size_t Len) {
  return SymbolPool.probeSymbol(Str, Len);
}

void freeSymbol(WASMSymbol Symbol) { return SymbolPool.freeSymbol(Symbol); }

const char *dumpSymbolString(WASMSymbol Symbol) {
  return SymbolPool.dumpSymbolString(Symbol);
}
```

## 9. 调试支持

`Runtime`类提供了调试支持，包括CPU跟踪和统计信息：

```cpp
void startCPUTracing() {
  if (!Config.EnableGdbTracingHook) {
    return;
  }
  // 使用系统调用写入跟踪标记...
}

void endCPUTracing() {
  if (!Config.EnableGdbTracingHook) {
    return;
  }
  // 使用系统调用写入跟踪标记...
}

utils::Statistics &getStatistics() { return Stats; }
```

## 10. 在DTVM中的应用

`Runtime`类是DTVM的核心组件，它被广泛应用于以下场景：

### 10.1 命令行工具

在DTVM命令行工具中，`Runtime`类用于加载和执行WebAssembly模块：

```cpp
std::unique_ptr<Runtime> RT = Runtime::newRuntime(Config);
RT->setWASIArgs(WasmFilename, Args);
RT->setWASIEnvs(Envs);
RT->setWASIDirs(Dirs);
HostModule *WASIMod = LOAD_HOST_MODULE(RT, zen::host, wasi_snapshot_preview1);
HostModule *EnvMod = LOAD_HOST_MODULE(RT, zen::host, env);
MayBe<Module *> ModRet = RT->loadModule(WasmFilename, ActualEntryHint);
Module *Mod = *ModRet;
Isolation *Iso = RT->createManagedIsolation();
MayBe<Instance *> InstRet = Iso->createInstance(*Mod, GasLimit);
Instance *Inst = *InstRet;
bool CallRet = RT->callWasmFunction(*Inst, FuncName, Args, Results);
```

### 10.2 Rust接口

在DTVM的Rust接口中，`Runtime`类被封装为`ZenRuntime`类：

```rust
// 创建运行时
let rt = ZenRuntime::new(None);
// 加载WebAssembly模块
let wasm_path = "path/to/module.wasm";
let wasm_mod = rt.load_module(wasm_path).unwrap();
// 创建隔离环境
let isolation = rt.new_isolation().unwrap();
// 创建WebAssembly实例
let gas_limit: u64 = 100000000;
let inst = wasm_mod.new_instance(isolation, gas_limit).unwrap();
// 调用WebAssembly函数
let args = vec![ZenValue::ZenI32Value(42)];
let results = inst.call_wasm_func("example_function", &args).unwrap();
```

## 11. 总结

`runtime.h`和`runtime.cpp`实现了DTVM项目的核心运行时系统，提供了WebAssembly模块的加载、编译和执行功能。这个系统是DTVM的核心组件，为整个虚拟机提供了基础设施和执行环境。

运行时系统的主要功能包括：

1. **模块管理**：加载和卸载WebAssembly模块，支持从文件或内存加载

2. **隔离环境管理**：创建和管理隔离环境，确保不同实例之间相互隔离

3. **函数调用**：支持按名称或索引调用WebAssembly函数，并处理参数和返回值

4. **多种执行模式**：支持解释器模式和JIT模式，满足不同的性能和资源需求

5. **WASI支持**：提供WebAssembly系统接口，允许WebAssembly模块访问系统资源

6. **内存和符号管理**：提供高效的内存和符号管理机制

7. **调试支持**：提供CPU跟踪和统计信息，帮助调试和性能分析

通过这些功能，DTVM能够高效地管理WebAssembly模块的生命周期，提供安全的执行环境，并支持与主机环境的交互。运行时系统的设计考虑了灵活性、性能和安全性，使其能够适应不同的应用场景和系统环境。

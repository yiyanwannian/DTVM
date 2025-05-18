# DTVM隔离环境系统（isolation.h/cpp）详解

`isolation.h`和`isolation.cpp`文件实现了DTVM项目的隔离环境系统，负责提供WebAssembly执行的隔离，确保不同WebAssembly实例之间相互隔离。这个系统是DTVM安全执行模型的重要组成部分。

## 1. Isolation类概述

`Isolation`类是DTVM隔离环境系统的核心，它为WebAssembly模块的实例提供了隔离的执行环境。

```cpp
class Isolation : public RuntimeObject<Isolation> {
  using Error = common::Error;
  using ErrorCode = common::ErrorCode;
  using ErrorPhase = common::ErrorPhase;
  friend class RuntimeObjectDestroyer;

public:
  static IsolationUniquePtr newIsolation(Runtime &RT) noexcept;

  common::MayBe<Instance *> createInstance(Module &Mod,
                                           uint64_t GasLimit = 0) noexcept;

  bool deleteInstance(Instance *Inst) noexcept;

  bool initWasi();
  bool initNativeModuleCtx(WASMSymbol ModName);

private:
  explicit Isolation(Runtime &RT) : RuntimeObject<Isolation>(RT) {}

  WNIEnvInternal WniEnv;

  std::unordered_map<Instance *, InstanceUniquePtr> InstancePool;
};
```

`Isolation`类继承自`RuntimeObject<Isolation>`，这是一个模板基类，提供了与运行时的关联。主要成员包括：

- **WniEnv**：WebAssembly原生接口环境，用于与主机环境交互
- **InstancePool**：实例池，存储由该隔离环境创建的所有WebAssembly实例

## 2. 隔离环境创建

### 2.1 创建隔离环境

```cpp
IsolationUniquePtr Isolation::newIsolation(Runtime &RT) noexcept {
  void *Buf = RT.allocate(sizeof(Isolation));
  ZEN_ASSERT(Buf);
  IsolationUniquePtr Iso(new (Buf) Isolation(RT));
  return Iso;
}
```

这个静态工厂方法创建一个新的隔离环境实例。它使用运行时的内存分配器分配内存，然后在该内存上构造一个`Isolation`对象。这种内存管理方式称为"placement new"，它允许在预先分配的内存上构造对象。

返回的`IsolationUniquePtr`是一个智能指针，它会在不再需要时自动销毁隔离环境对象。

## 3. 实例管理

### 3.1 创建WebAssembly实例

```cpp
common::MayBe<Instance *>
Isolation::createInstance(Module &Mod, uint64_t GasLimit) noexcept {
#ifdef ZEN_ENABLE_BUILTIN_WASI
  // disable it temporally
  // if (!initWasi()) {
  //    return nullptr;
  //}
#endif

  InstanceUniquePtr Inst;

  auto &Stats = getRuntime()->getStatistics();
  auto Timer = Stats.startRecord(utils::StatisticPhase::Instantiation);
  try {
    Inst = Instance::newInstance(*this, Mod, GasLimit);
  } catch (const Error &Err) {
    Stats.clearAllTimers();
    return Err;
  }
  Stats.stopRecord(Timer);
  ZEN_ASSERT(Inst);

  Instance *RawInst = Inst.get();
  auto EmplaceRet =
      InstancePool.emplace(RawInst, std::forward<InstanceUniquePtr>(Inst));
  if (!EmplaceRet.second) {
    return nullptr;
  }

  WNIEnv *Env = reinterpret_cast<WNIEnv *>(&WniEnv);
  Env->_functions = wni_functions();
  if (RawInst->hasMemory()) {
    const auto &MemInst = RawInst->getDefaultMemoryInst();
    Env->_linear_mem_base = reinterpret_cast<uintptr_t>(MemInst.MemBase);
    Env->_linear_mem_size = MemInst.MemSize;
    Env->_linear_mem_end = Env->_linear_mem_base + Env->_linear_mem_size;
  } else {
    Env->_linear_mem_base = 0;
    Env->_linear_mem_size = 0;
    Env->_linear_mem_end = 0;
  }

  return RawInst;
}
```

这个方法创建一个WebAssembly模块的新实例，并将其添加到实例池中。主要步骤包括：

1. **开始记录实例化时间**：使用统计系统记录实例化过程的时间
2. **创建实例**：调用`Instance::newInstance`创建一个新的实例，传入隔离环境、模块和Gas限制
3. **处理异常**：如果实例化过程中发生错误，清除计时器并返回错误
4. **停止记录实例化时间**：记录实例化完成的时间
5. **添加到实例池**：将新创建的实例添加到实例池中
6. **设置WNI环境**：配置WebAssembly原生接口环境，包括函数表和线性内存信息
7. **返回实例指针**：返回新创建的实例的原始指针

返回的`MayBe<Instance *>`是一个结果包装器，它可能包含一个实例指针或一个错误。

### 3.2 删除WebAssembly实例

```cpp
bool Isolation::deleteInstance(Instance *Inst) noexcept {
  return InstancePool.erase(Inst) != 0;
}
```

这个方法从实例池中删除指定的实例。当实例从池中移除时，对应的`InstanceUniquePtr`会自动销毁实例对象。

## 4. WASI和原生模块支持

### 4.1 初始化WASI

```cpp
bool Isolation::initWasi() {
  return initNativeModuleCtx(common::WASM_SYMBOL_wasi_snapshot_preview1);
}
```

这个方法初始化WASI（WebAssembly系统接口）环境。它调用`initNativeModuleCtx`方法，传入WASI模块的符号名称。

### 4.2 初始化原生模块上下文

```cpp
bool Isolation::initNativeModuleCtx(WASMSymbol ModName) {
  HostModule *HostMod = getRuntime()->resolveHostModule(ModName);
  if (!HostMod) {
    return false;
  }

  const auto &FuncList = HostMod->getHostFuntionList();
  uint32_t NumHostFunctions = HostMod->getNumHostFunctions();
  if (!NumHostFunctions) {
    return false;
  }

  // check if initCtx exists and signature is valid
  uint32_t InitCtxFuncId = 0;
  for (InitCtxFuncId = 0; InitCtxFuncId < NumHostFunctions; ++InitCtxFuncId) {
    if (FuncList[InitCtxFuncId]->_name == common::WASM_SYMBOL_init_ctx) {
      break;
    }
  }

  if (InitCtxFuncId >= NumHostFunctions) {
    return true; // not necessary to call initCtx for this module. it's not
                 // an error.
  }

  if (!FuncList[InitCtxFuncId]->_isReserved ||
      FuncList[InitCtxFuncId]->_param_count != 6 ||
      FuncList[InitCtxFuncId]->_ret_count != 1) {
    // not match initCtx signature, it's a user defined initCtx, not
    // reserved initCtx.
    return true;
  }

  // call initCtx
  VNMI_RESERVED_INIT_CTX_TYPE InitCtxFn =
      reinterpret_cast<VNMI_RESERVED_INIT_CTX_TYPE>(
          FuncList[InitCtxFuncId]->_ptr);
  void *Ctx = InitCtxFn(HostMod->getVNMIEnv(), nullptr, 0, nullptr, 0, nullptr,
                        0, nullptr, 0, nullptr, 0);

  // note: for simplicity, currently only supports one WNIEnv and only one
  // user defined ctx certainly.
  WNIEnv *Env = reinterpret_cast<WNIEnv *>(&WniEnv);
  Env->_user_defined_ctx = Ctx;

  return true;
}
```

这个方法初始化指定的原生模块上下文。主要步骤包括：

1. **解析主机模块**：通过运行时解析指定名称的主机模块
2. **获取主机函数列表**：获取模块中的主机函数列表
3. **查找初始化上下文函数**：在函数列表中查找名为`init_ctx`的函数
4. **验证函数签名**：检查函数是否为保留函数，并验证其参数和返回值数量
5. **调用初始化函数**：调用`init_ctx`函数初始化模块上下文
6. **设置用户定义上下文**：将初始化函数返回的上下文设置到WNI环境中

## 5. 隔离环境的作用

隔离环境在DTVM中扮演着重要的角色，它提供了以下关键功能：

### 5.1 实例隔离

隔离环境确保不同的WebAssembly实例之间相互隔离，防止一个实例影响另一个实例。这对于安全执行多个WebAssembly模块至关重要，特别是在多租户环境中。

### 5.2 资源管理

隔离环境管理其创建的所有实例，确保资源的正确分配和释放。当隔离环境被销毁时，它会自动销毁所有关联的实例。

### 5.3 主机接口

隔离环境提供了WebAssembly与主机环境交互的接口。通过WNI环境，WebAssembly实例可以调用主机函数，访问主机资源。

## 6. 在DTVM中的应用

隔离环境在DTVM中的应用非常广泛，以下是一些典型场景：

### 6.1 命令行工具

在DTVM命令行工具中，隔离环境用于创建和管理WebAssembly实例：

```cpp
// 创建运行时
std::unique_ptr<Runtime> RT = Runtime::newRuntime(Config);
// 创建隔离环境
Isolation *Iso = RT->createManagedIsolation();
// 创建WebAssembly实例
MayBe<Instance *> InstRet = Iso->createInstance(*Mod, GasLimit);
Instance *Inst = *InstRet;
// 调用WebAssembly函数
bool CallRet = RT->callWasmFunction(*Inst, FuncName, Args, Results);
```

### 6.2 Rust接口

在DTVM的Rust接口中，隔离环境被封装为`ZenIsolation`类：

```rust
// 创建运行时
let rt = ZenRuntime::new(None);
// 创建隔离环境
let isolation = rt.new_isolation().unwrap();
// 创建WebAssembly实例
let gas_limit: u64 = 100000000;
let inst = wasm_mod.new_instance(isolation, gas_limit).unwrap();
```

## 7. 隔离环境与安全性

隔离环境是DTVM安全模型的核心组件，它通过以下机制提供安全保障：

### 7.1 内存隔离

每个WebAssembly实例都有自己的内存空间，隔离环境确保一个实例不能访问另一个实例的内存。

### 7.2 资源限制

隔离环境支持设置Gas限制，防止WebAssembly代码无限消耗资源。当Gas耗尽时，执行会被终止。

### 7.3 主机函数控制

隔离环境控制WebAssembly实例可以调用的主机函数，确保只有安全的函数可以被调用。

## 8. 与其他组件的关系

隔离环境与DTVM的其他组件有着密切的关系：

### 8.1 与Runtime的关系

隔离环境由Runtime创建和管理。Runtime可以创建多个隔离环境，每个隔离环境都与创建它的Runtime关联。

```cpp
// 在Runtime中创建隔离环境
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

### 8.2 与Module的关系

隔离环境不直接与Module关联，但它可以创建Module的实例。一个Module可以在多个隔离环境中实例化。

### 8.3 与Instance的关系

隔离环境创建和管理Instance。每个Instance都与创建它的隔离环境关联，并且只能在该隔离环境中执行。

```cpp
// 在Instance构造函数中设置隔离环境
Instance::Instance(Isolation &Iso, const Module &Mod, uint64_t GasLimit)
    : RuntimeObject<Instance>(*Iso.getRuntime()), Iso(&Iso), Mod(&Mod),
      GasLimit(GasLimit) {}
```

## 总结

`isolation.h`和`isolation.cpp`实现了DTVM项目的隔离环境系统，提供了WebAssembly执行的隔离和安全保障。隔离环境是DTVM安全执行模型的核心组件，它确保不同WebAssembly实例之间相互隔离，防止恶意代码影响系统安全。通过隔离环境，DTVM能够安全地执行多个WebAssembly模块，为区块链、边缘计算和安全计算等场景提供可靠的执行环境。

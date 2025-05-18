# DTVM项目主要对象列表

DTVM项目中的main函数使用了多个关键对象来实现WebAssembly模块的加载、编译和执行。以下是这些主要对象的详细列表，分别从C++命令行工具和Rust示例两个入口点进行分析。

## C++命令行工具中的主要对象 (src/cli/dtvm.cpp)

### 1. 命令行解析相关对象

| 对象名称 | 类型 | 源码位置 | 功能描述 |
|---------|------|---------|---------|
| `CLIParser` | `std::unique_ptr<CLI::App>` | 第55行 | 命令行参数解析器，用于解析和处理命令行参数 |
| `WasmFilename` | `std::string` | 第65行 | 存储WebAssembly文件路径 |
| `FuncName` | `std::string` | 第66行 | 存储要执行的函数名 |
| `Args` | `std::vector<std::string>` | 第68行 | 存储传递给WebAssembly函数的参数列表 |
| `Envs` | `std::vector<std::string>` | 第69行 | 存储WASI环境变量列表 |
| `Dirs` | `std::vector<std::string>` | 第70行 | 存储WASI工作目录列表 |
| `GasLimit` | `uint64_t` | 第71行 | 存储执行的gas限制，用于防止无限循环 |
| `LogLevel` | `LoggerLevel` | 第72行 | 存储日志级别 |

### 2. 配置相关对象

| 对象名称 | 类型 | 源码位置 | 功能描述 |
|---------|------|---------|---------|
| `Config` | `RuntimeConfig` | 第75行 | 运行时配置对象，包含多种配置选项 |
| `ModeMap` | `std::unordered_map<std::string, RunMode>` | 第79行 | 运行模式映射表，将字符串映射到枚举值 |
| `LogMap` | `std::unordered_map<std::string, LoggerLevel>` | 第86行 | 日志级别映射表，将字符串映射到枚举值 |

### 3. 运行时相关对象

| 对象名称 | 类型 | 源码位置 | 功能描述 |
|---------|------|---------|---------|
| `RT` | `std::unique_ptr<Runtime>` | 第157行 | 运行时环境对象，DTVM的核心组件 |
| `WASIMod` | `HostModule*` | 第168行 | WASI模块对象，提供WebAssembly系统接口 |
| `EnvMod` | `HostModule*` | 第176行 | 环境模块对象，提供基本的主机环境函数 |

### 4. WebAssembly执行相关对象

| 对象名称 | 类型 | 源码位置 | 功能描述 |
|---------|------|---------|---------|
| `ModRet` | `MayBe<Module*>` | 第185行 | 模块加载结果，包含加载的模块或错误信息 |
| `Mod` | `Module*` | 第193行 | WebAssembly模块对象，表示加载的模块 |
| `Iso` | `Isolation*` | 第196行 | 隔离环境对象，提供执行隔离 |
| `InstRet` | `MayBe<Instance*>` | 第203行 | 实例创建结果，包含创建的实例或错误信息 |
| `Inst` | `Instance*` | 第211行 | WebAssembly实例对象，表示实例化的模块 |
| `Results` | `std::vector<TypedValue>` | 第214行 | 存储函数调用结果 |
| `Code` | `CodeHolderUniquePtr` | 第245行 | 代码持有器对象，用于基准测试 |

## Rust示例中的主要对象 (rust_crate/rust_example/src/main.rs)

| 对象名称 | 类型 | 源码位置 | 功能描述 |
|---------|------|---------|---------|
| `rt` | `ZenRuntime` | 第8行 | Rust运行时对象，DTVM的Rust接口核心组件 |
| `wasm_path` | `&str` | 第9行 | WebAssembly文件路径 |
| `maybe_mod` | `Result<ZenModule, String>` | 第11行 | 模块加载结果，包含加载的模块或错误信息 |
| `wasm_mod` | `ZenModule` | 第17行 | WebAssembly模块对象，表示加载的模块 |
| `isolation` | `Result<ZenIsolation, String>` | 第18行 | 隔离环境创建结果 |
| `gas_limit` | `u64` | 第24行 | 执行的gas限制，用于防止无限循环 |
| `maybe_inst` | `Result<ZenInstance, String>` | 第25行 | 实例创建结果 |
| `inst` | `ZenInstance` | 第31行 | WebAssembly实例对象，表示实例化的模块 |
| `args` | `Vec<ZenValue>` | 第32行 | 函数参数列表，包含传递给WebAssembly函数的参数 |
| `results` | `Result<Vec<ZenValue>, String>` | 第33行 | 函数调用结果 |
| `result` | `&ZenValue` | 第38行 | 函数返回值 |

## 对象关系图

```
C++命令行工具对象关系:
CLIParser
    |
    v
Config --> RT (Runtime) --> WASIMod, EnvMod
                |
                v
            ModRet --> Mod
                        |
                        v
                    Iso --> InstRet --> Inst
                                        |
                                        v
                                    Results

Rust示例对象关系:
rt (ZenRuntime)
    |
    |----> maybe_mod --> wasm_mod
    |                       |
    |----> isolation        |
            |               |
            v               v
        maybe_inst --> inst --> results --> result
```

## 主要对象功能总结

1. **运行时对象** (RT/rt)：DTVM的核心组件，负责管理整个WebAssembly执行环境。

2. **模块对象** (Mod/wasm_mod)：表示加载的WebAssembly模块，包含模块的类型、函数、内存、表等信息。

3. **隔离环境对象** (Iso/isolation)：提供执行隔离，确保不同WebAssembly实例之间相互隔离。

4. **实例对象** (Inst/inst)：表示实例化的WebAssembly模块，包含运行时状态，如内存、全局变量等。

5. **配置对象** (Config)：包含运行时的各种配置选项，如运行模式、内存映射、统计功能等。

6. **主机模块对象** (WASIMod/EnvMod)：提供WebAssembly与主机环境交互的接口，如文件系统访问、环境变量等。

这些对象共同构成了DTVM的执行流程：从加载WebAssembly模块，到创建隔离环境，再到实例化模块，最后执行指定的函数并获取结果。

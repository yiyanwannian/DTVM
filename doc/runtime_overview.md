# DTVM Runtime系统概述

DTVM的Runtime系统是整个虚拟机的核心，负责WebAssembly模块的加载、编译、实例化和执行。本文档基于源码分析，详细讲解`src/runtime`目录下的设计和运行逻辑。

## 1. 系统架构

DTVM Runtime系统采用分层设计，主要包括以下几个核心组件：

1. **Runtime**：运行时环境，管理整个虚拟机的生命周期
2. **Isolation**：隔离环境，提供WebAssembly实例的隔离执行
3. **Module**：模块，表示加载的WebAssembly模块
4. **Instance**：实例，表示WebAssembly模块的运行实例
5. **Memory**：内存管理，负责WebAssembly线性内存的分配和管理
6. **接口层**：包括VNMI和WNI，提供与主机环境的交互

这些组件之间的关系如下：

- Runtime创建和管理多个Isolation
- Isolation创建和管理多个Instance
- Instance关联一个Module
- Module包含代码、数据、函数等信息
- Memory管理WebAssembly的线性内存
- VNMI和WNI提供与主机环境的交互接口

## 2. 核心组件详解

### 2.1 Runtime (runtime.h/cpp)

Runtime是DTVM的核心组件，负责管理整个虚拟机的生命周期。

**主要职责**：
- 创建和管理隔离环境(Isolation)
- 加载WebAssembly模块
- 加载主机模块
- 管理内存和符号
- 提供WebAssembly函数调用接口
- 支持WASI环境

**关键方法**：
- `newRuntime()`：创建新的Runtime实例
- `loadModule()`：加载WebAssembly模块
- `createManagedIsolation()`：创建隔离环境
- `callWasmFunction()`：调用WebAssembly函数

### 2.2 Isolation (isolation.h/cpp)

Isolation提供WebAssembly实例的隔离执行环境，确保不同实例之间相互隔离。

**主要职责**：
- 创建和管理WebAssembly实例
- 提供实例间的隔离
- 初始化WASI环境
- 管理原生模块上下文

**关键方法**：
- `newIsolation()`：创建新的隔离环境
- `createInstance()`：创建WebAssembly实例
- `deleteInstance()`：删除WebAssembly实例
- `initWasi()`：初始化WASI环境

### 2.3 Module (module.h/cpp)

Module表示加载的WebAssembly模块，包含代码、数据、函数等信息。

**主要职责**：
- 存储WebAssembly模块的代码和数据
- 管理导入和导出函数
- 管理类型、函数、表、内存和全局变量
- 计算实例布局

**关键方法**：
- `newModule()`：创建新的Module实例
- `getExportFunc()`：获取导出函数
- `getImportFunc()`：获取导入函数
- `getLayout()`：获取实例布局

### 2.4 Instance (instance.h/cpp)

Instance表示WebAssembly模块的运行实例，包含运行时状态。

**主要职责**：
- 存储实例的运行时状态
- 管理函数、表、内存和全局变量实例
- 执行WebAssembly代码
- 处理异常和错误

**关键方法**：
- `newInstance()`：创建新的Instance实例
- `getFunctionInst()`：获取函数实例
- `getTableInst()`：获取表实例
- `getMemoryInst()`：获取内存实例
- `getGlobalInst()`：获取全局变量实例

### 2.5 Memory (memory.h/cpp)

Memory负责WebAssembly线性内存的分配和管理。

**主要职责**：
- 分配和管理WebAssembly线性内存
- 支持内存增长
- 提供内存保护
- 支持内存桶模式

**关键方法**：
- `allocInitWasmMemory()`：分配初始内存
- `enlargeWasmMemory()`：扩展内存
- `freeWasmMemory()`：释放内存
- `mprotectReadWriteWasmMemoryData()`：设置内存保护

### 2.6 接口层 (vnmi.h/cpp, wni.h/cpp)

接口层提供与主机环境的交互接口。

**VNMI (Virtual Native Machine Interface)**：
- 提供内存分配和释放
- 提供符号管理
- 连接Runtime和主机模块

**WNI (WebAssembly Native Interface)**：
- 提供地址转换
- 提供地址验证
- 提供异常处理
- 提供上下文管理

## 3. 运行流程

DTVM Runtime系统的典型运行流程如下：

1. **创建Runtime**：调用`Runtime::newRuntime()`创建Runtime实例
2. **加载模块**：调用`Runtime::loadModule()`加载WebAssembly模块
3. **创建隔离环境**：调用`Runtime::createManagedIsolation()`创建隔离环境
4. **创建实例**：调用`Isolation::createInstance()`创建WebAssembly实例
5. **调用函数**：调用`Runtime::callWasmFunction()`执行WebAssembly函数
6. **清理资源**：销毁实例、隔离环境和Runtime

## 4. 内存管理

DTVM采用多种内存管理策略：

1. **内存桶模式**：预分配大块内存，划分为多个固定大小的桶项
2. **单一映射模式**：为每个实例分配单独的内存映射
3. **普通内存模式**：使用标准内存分配函数

内存桶模式是DTVM的主要内存管理策略，它提供了内存保护和CPU异常检测功能，同时支持高效的内存分配和释放。

## 5. 并发支持

DTVM通过以下机制支持并发执行：

1. **多线程并行编译**：使用线程池并行编译WebAssembly函数
2. **延迟编译模式**：只在需要时才编译函数
3. **多种执行模式**：支持解释器模式、单通道编译模式和多通道编译模式
4. **多实例部署**：支持部署多个DTVM实例，每个实例有自己的内存桶

## 6. 安全机制

DTVM实现了多种安全机制：

1. **实例隔离**：不同实例之间相互隔离，防止相互干扰
2. **内存保护**：使用内存映射和保护机制，防止非法内存访问
3. **CPU异常检测**：捕获CPU异常，防止崩溃
4. **Gas计量**：限制执行资源，防止无限循环

## 总结

DTVM Runtime系统是一个复杂而强大的WebAssembly虚拟机实现，它提供了高效、安全、灵活的WebAssembly执行环境。通过分层设计和模块化架构，DTVM能够支持各种WebAssembly应用场景，特别是在区块链和安全计算领域。

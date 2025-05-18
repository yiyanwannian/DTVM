# DTVM Rust接口说明

## 简介

DTVM（ZetaEngine）提供了Rust语言的接口，使开发者能够在Rust应用程序中嵌入和运行WebAssembly模块。本文档详细介绍了DTVM的Rust接口的使用方法和主要组件。

## 安装与配置

要在Rust项目中使用DTVM，需要在`Cargo.toml`中添加以下依赖：

```toml
[dependencies]
dtvmcore_rust = { path = "路径到/DTVM/rust_crate" }
cty = "0.2.2"
libc = { version = "0.2.121" }
```

## 核心组件

DTVM的Rust接口主要包含以下核心组件：

### 1. ZenRuntime

`ZenRuntime`是DTVM的运行时环境，负责管理WebAssembly模块的加载和执行。

```rust
use dtvmcore_rust::core::runtime::ZenRuntime;

// 创建一个新的运行时
let rt = ZenRuntime::new(None);
```

主要方法：
- `new(config: Option<ZenRuntimeConfig>)` - 创建一个新的运行时
- `load_module(path: &str)` - 从文件加载WebAssembly模块
- `load_module_from_bytes(name: &str, bytes: &[u8])` - 从内存中的字节数组加载WebAssembly模块
- `new_isolation()` - 创建一个新的隔离环境

### 2. ZenModule

`ZenModule`表示一个已加载的WebAssembly模块。

```rust
// 从文件加载模块
let maybe_mod = rt.load_module("path/to/module.wasm");
if let Ok(wasm_mod) = maybe_mod {
    // 使用模块...
}
```

主要方法：
- `new_instance(isolation: ZenIsolation, gas_limit: u64)` - 创建模块的新实例

### 3. ZenIsolation

`ZenIsolation`提供了WebAssembly执行的隔离环境。

```rust
// 创建隔离环境
let isolation = rt.new_isolation();
if let Ok(iso) = isolation {
    // 使用隔离环境...
}
```

### 4. ZenInstance

`ZenInstance`是WebAssembly模块的运行实例。

```rust
// 创建实例
let gas_limit: u64 = 100000000;
let maybe_inst = wasm_mod.new_instance(isolation, gas_limit);
if let Ok(inst) = maybe_inst {
    // 使用实例...
}
```

主要方法：
- `call_wasm_func(func_name: &str, args: &[ZenValue])` - 调用WebAssembly函数
- `validate_wasm_addr(addr: u32, size: u32)` - 验证WebAssembly内存地址是否有效
- `get_host_memory(addr: u32)` - 获取WebAssembly内存的主机指针

### 5. ZenValue

`ZenValue`用于表示WebAssembly函数的参数和返回值。

```rust
use dtvmcore_rust::core::types::ZenValue;

// 创建参数
let args = vec![
    ZenValue::ZenI32Value(42),
    ZenValue::ZenF64Value(3.14)
];
```

支持的类型：
- `ZenI32Value(i32)` - 32位整数
- `ZenI64Value(i64)` - 64位整数
- `ZenF32Value(f32)` - 32位浮点数
- `ZenF64Value(f64)` - 64位浮点数

## 主机函数注册

DTVM允许注册主机函数，使WebAssembly模块能够调用Rust函数。

```rust
use dtvmcore_rust::core::host_module::*;

// 定义主机函数
extern "C" fn get_host_number(wasm_inst: *mut ZenInstanceExtern, a: i32, b: i32) -> i32 {
    // 实现...
    return 42;
}

// 创建主机模块描述
let host_func_descs = vec![
    ZenHostFuncDesc {
        name: "get_host_number".to_string(),
        arg_types: vec![ZenType::ZenTypeI32, ZenType::ZenTypeI32],
        ret_types: vec![ZenType::ZenTypeI32],
        func_ptr: get_host_number as *const libc::c_void,
    },
];

// 创建主机模块
let host_module_desc = rt.create_host_module_desc("env", host_func_descs);
let host_module = rt.load_host_module(host_module_desc);
```

## 完整示例

以下是一个完整的示例，展示了如何加载WebAssembly模块并调用其中的函数：

```rust
use dtvmcore_rust::core::runtime::ZenRuntime;
use dtvmcore_rust::core::types::ZenValue;

fn main() {
    // 创建运行时
    let rt = ZenRuntime::new(None);
    
    // 加载WebAssembly模块
    let wasm_path = "path/to/module.wasm";
    let maybe_mod = rt.load_module(wasm_path);
    if let Err(err) = maybe_mod {
        println!("加载模块错误: {err}");
        return;
    }
    let wasm_mod = maybe_mod.unwrap();
    
    // 创建隔离环境
    let isolation = rt.new_isolation();
    if let Err(err) = isolation {
        println!("创建隔离环境错误: {err}");
        return;
    }
    let isolation = isolation.unwrap();
    
    // 设置gas限制
    let gas_limit: u64 = 100000000;
    
    // 创建WebAssembly实例
    let maybe_inst = wasm_mod.new_instance(isolation, gas_limit);
    if let Err(err) = maybe_inst {
        println!("创建WebAssembly实例错误: {err}");
        return;
    }
    let inst = maybe_inst.unwrap();
    
    // 准备参数并调用WebAssembly函数
    let args = vec![ZenValue::ZenI32Value(42)];
    let results = inst.call_wasm_func("example_function", &args);
    if let Err(err) = results {
        println!("调用WebAssembly函数错误: {err}");
        return;
    }
    
    // 获取并处理结果
    let result = &results.unwrap()[0];
    println!("WebAssembly函数结果: {result}");
}
```

## 高级用法

### 从内存加载WebAssembly模块

```rust
use std::fs;

let wasm_path = "path/to/module.wasm";
let wasm_bytes = fs::read(wasm_path).unwrap();
let maybe_mod = rt.load_module_from_bytes(wasm_path, &wasm_bytes);
```

### 访问WebAssembly内存

```rust
let inst: &ZenInstance<i64> = ZenInstance::from_raw_pointer(wasm_inst);
if inst.validate_wasm_addr(0, 1) {
    let memory_addr: *const u8 = inst.get_host_memory(0);
    let memory_value = unsafe { *memory_addr } as i32;
    // 使用内存值...
}
```

## 错误处理

DTVM的Rust接口使用`Result`类型进行错误处理。在使用接口时，应当始终检查返回的`Result`是否为`Ok`。

```rust
let maybe_mod = rt.load_module(wasm_path);
match maybe_mod {
    Ok(wasm_mod) => {
        // 使用模块...
    },
    Err(err) => {
        println!("加载模块错误: {err}");
        // 处理错误...
    }
}
```

## 性能考虑

- 设置适当的gas限制以防止无限循环
- 对于频繁调用的WebAssembly函数，考虑重用同一个实例
- 对于大型WebAssembly模块，考虑使用`load_module_from_bytes`预加载模块

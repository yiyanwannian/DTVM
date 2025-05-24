# NumImportFunctions 和 NumInternalFunctions 计算详解

## 概述

`NumImportFunctions` 和 `NumInternalFunctions` 是 DTVM 中 `Module` 类的两个重要成员变量，它们分别表示：

- **NumImportFunctions**：从其他模块导入的函数数量
- **NumInternalFunctions**：模块内部定义的函数数量

这两个值在 WebAssembly 模块加载过程中通过解析 WASM 字节码计算得出，用于确定实例的内存布局。

## 计算过程

### 1. NumImportFunctions 的计算

`NumImportFunctions` 在解析 **Import Section** 时计算：

```cpp
void ModuleLoader::loadImportSection() {
  uint32_t NumImports = readU32();                             // 从WASM字节码读取导入项总数
  
  for (uint32_t I = 0; I < NumImports; ++I) {
    WASMSymbol ModuleName = readName();                        // 读取模块名
    WASMSymbol FieldName = readName();                         // 读取字段名
    
    uint8_t ImportKind = to_underlying(readByte());            // 读取导入类型
    switch (ImportKind) {
    case IMPORT_FUNC: {                                        // 如果是函数导入
      uint32_t TypeIdx = readU32();                           // 读取函数类型索引
      // 验证类型索引有效性
      if (!Mod.isValidType(TypeIdx)) {
        throw getError(ErrorCode::UnknownTypeIdx);
      }
      
      // 解析导入函数并添加到导入函数表
      ImportFunctionTable.push_back(ImportFunctionEntry{
        .ModuleName = ModuleName,
        .FieldName = FieldName,
        .TypeIdx = TypeIdx,
        // ...
      });
      break;
    }
    case IMPORT_TABLE:
      // 处理表导入
      break;
    case IMPORT_MEMORY:
      // 处理内存导入
      break;
    case IMPORT_GLOBAL:
      // 处理全局变量导入
      break;
    }
  }
  
  // 计算导入函数总数
  uint32_t NumImportFuncs = static_cast<uint32_t>(ImportFunctionTable.size());
  
  // 通过 initImportFuncTable 设置 Mod.NumImportFunctions
  Mod.initImportFuncTable(NumImportFuncs);
}
```

### 2. NumInternalFunctions 的计算

`NumInternalFunctions` 在解析 **Function Section** 时直接读取：

```cpp
void ModuleLoader::loadFunctionSection() {
  uint32_t NumFunctions = readU32();                           // 从WASM字节码直接读取内部函数数量
  
  // 验证函数数量不超过限制
  uint32_t TotalNumFunctions;
  if (addOverflow(NumFunctions, Mod.NumImportFunctions, TotalNumFunctions)) {
    throw getError(ErrorCode::TooManyFunctions);
  }
  if (TotalNumFunctions > PresetMaxNumFunctions) {
    throw getError(ErrorCode::TooManyFunctions);
  }

  // 初始化内部函数表，同时设置 NumInternalFunctions
  FuncEntry *Entry = Mod.initFuncTable(NumFunctions);
  
  // 处理每个内部函数的类型信息
  for (uint32_t I = 0; I < NumFunctions; ++I) {
    uint32_t TypeIdx = readU32();                              // 读取函数类型索引
    if (!Mod.isValidType(TypeIdx)) {
      throw getError(ErrorCode::UnknownTypeIdx);
    }

    TypeEntry *Type = Mod.getDeclaredType(TypeIdx);
    Entry->OriginTypeIdx = TypeIdx;
    Entry->TypeIdx = Type->SmallestTypeIdx;
    
    ++Entry;
  }
}
```

### 3. 内部实现机制

在 `Module` 类中，这些值通过模板方法 `initItemTable` 设置：

```cpp
template <typename EntryType>
EntryType *initItemTable(EntryType *&ItemTable, uint32_t &NumItems, uint32_t InitNumItems) {
  size_t Size = sizeof(EntryType) * InitNumItems;
  if (Size > UINT32_MAX) {
    throw common::getError(common::ErrorCode::TooManyItems);
  }
  EntryType *Entry = Size ? (EntryType *)allocateZeros(Size) : nullptr;
  NumItems = InitNumItems;                                     // 设置数量
  ItemTable = Entry;                                           // 设置表指针
  return Entry;
}

// 设置导入函数数量
ImportFunctionEntry *initImportFuncTable(uint32_t N) {
  return initItemTable(ImportFunctionTable, NumImportFunctions, N);
}

// 设置内部函数数量
FuncEntry *initFuncTable(uint32_t N) {
  return initItemTable(InternalFunctionTable, NumInternalFunctions, N);
}
```

## 数据来源

这些数值最终来源于 **WebAssembly 字节码文件** 的特定段：

### Import Section（导入段）
- **位置**：WASM 文件的第 2 个段（在 Type Section 之后）
- **内容**：包含所有导入项（函数、表、内存、全局变量）
- **格式**：
  ```
  import_section ::= section_2(vec(import))
  import ::= module:name field:name importdesc
  importdesc ::= func typeidx | table tabletype | mem memtype | global globaltype
  ```

### Function Section（函数段）
- **位置**：WASM 文件的第 3 个段（在 Import Section 之后）
- **内容**：内部函数的类型索引列表
- **格式**：
  ```
  function_section ::= section_3(vec(typeidx))
  ```

## 验证机制

### 1. 数量限制检查
```cpp
// 检查导入函数数量
if (NumImportFuncs > PresetMaxNumFunctions) {
  throw getError(ErrorCode::TooManyFunctions);
}

// 检查总函数数量
if (TotalNumFunctions > PresetMaxNumFunctions) {
  throw getError(ErrorCode::TooManyFunctions);
}
```

### 2. 一致性验证
```cpp
void ModuleLoader::loadCodeSection() {
  uint32_t NumCodes = readU32();
  // 验证代码段数量与内部函数数量一致
  if (NumCodes != Mod.NumInternalFunctions) {
    throw getError(ErrorCode::FuncCodeInconsistent);
  }
}
```

### 3. 模块加载完成后的最终检查
```cpp
void ModuleLoader::loadModuleBody() {
  // ... 加载各个段
  
  // 检查函数数量一致性
  if (Mod.NumInternalFunctions != Mod.NumCodeSegments) {
    throw getError(ErrorCode::FuncCodeInconsistent);
  }
}
```

## 使用场景

这两个值在系统中的主要用途：

### 1. 内存布局计算
```cpp
void Module::InstanceLayout::compute() {
  const uint32_t NumFunctions = Mod.getNumTotalFunctions();    // NumImportFunctions + NumInternalFunctions
  // 计算函数实例数组的大小
  FuncInstancesSize = ZEN_ALIGN(sizeof(FunctionInstance) * NumFunctions, Alignment);
}
```

### 2. 函数索引验证
```cpp
bool Module::isValidFunc(uint32_t FuncIdx) const {
  return FuncIdx < getNumTotalFunctions();                     // 验证函数索引是否有效
}
```

### 3. 函数类型查询
```cpp
uint32_t Module::getFunctionTypeIdx(uint32_t FuncIdx) const {
  if (FuncIdx < NumImportFunctions) {                          // 导入函数
    return getImportFunction(FuncIdx).TypeIdx;
  }
  uint32_t InternalFuncIdx = FuncIdx - NumImportFunctions;     // 内部函数
  return getInternalFunction(InternalFuncIdx).TypeIdx;
}
```

## 总结

- **NumImportFunctions**：通过解析 WASM 的 Import Section 计算，统计类型为函数的导入项数量
- **NumInternalFunctions**：通过解析 WASM 的 Function Section 直接读取，表示模块内部定义的函数数量
- **总函数数量**：`NumImportFunctions + NumInternalFunctions`
- **计算时机**：模块加载时确定，之后不会改变
- **主要用途**：用于计算实例的内存布局、函数索引验证和函数类型查询

这些值是 DTVM 运行时系统正确管理 WebAssembly 模块和实例的基础数据。

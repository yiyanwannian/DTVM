// Copyright (C) 2021-2023 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "action/interpreter.h"
#include "action/hook.h"
#include "common/errors.h"
#include "entrypoint/entrypoint.h"
#include "runtime/instance.h"
#include "utils/logging.h"
#include "utils/wasm.h"
#include <bitset>
#include <cmath>
#include <type_traits>

namespace zen::action {

using namespace common;
using namespace utils;
using namespace runtime;

//
// local_ptr <-----> frame <-----> control stack <----> value stack
// 栈帧内存布局：局部变量 -> 栈帧结构 -> 控制栈 -> 值栈
InterpFrame *InterpreterExecContext::allocFrame(FunctionInstance *FuncInst,
                                                uint32_t *LocalPtr) {  // 为函数调用分配新的栈帧
  InterpStack *Stack = getInterpStack();  // 获取解释器栈
  uint32_t LocalSize = FuncInst->NumLocalCells << 2;  // 计算局部变量所需空间（字节）
  uint32_t ControlSize = FuncInst->MaxBlockDepth * sizeof(BlockInfo);  // 计算控制栈所需空间
  // check stack overflow
  // 检查栈溢出：确保有足够空间分配局部变量、栈帧、控制栈和值栈
  if (Stack->top() + LocalSize + sizeof(InterpFrame) + FuncInst->MaxStackSize +
          ControlSize >=
      Stack->TopBoundary) {
    return nullptr;  // 栈空间不足，返回空指针
  }

  // alloc locals
  std::memset(Stack->top(), 0, LocalSize);  // 分配并清零局部变量空间
  Stack->Top += LocalSize;  // 移动栈顶指针

  // alloc Frame
  InterpFrame *Frame = (InterpFrame *)Stack->Top;  // 在当前栈顶分配栈帧结构
  std::memset(Stack->top(), 0, sizeof(InterpFrame));  // 清零栈帧结构
  Stack->Top += sizeof(InterpFrame);  // 移动栈顶指针

  // alloc control stack
  Frame->CtrlStackPtr = Frame->CtrlBasePtr = (BlockInfo *)Stack->top();  // 设置控制栈指针
  Stack->Top += ControlSize;  // 为控制栈分配空间
  Frame->CtrlBoundary = (BlockInfo *)Stack->top();  // 设置控制栈边界

  // alloc value stack
  Frame->ValueStackPtr = Frame->ValueBasePtr = (uint32_t *)Stack->top();  // 设置值栈指针
  Stack->Top += FuncInst->MaxStackSize;  // 为值栈分配空间
  Frame->ValueBoundary = (uint32_t *)Stack->top();  // 设置值栈边界

  Frame->LocalPtr = LocalPtr;  // 设置局部变量指针
  Frame->FuncInst = FuncInst;  // 设置函数实例指针
  Frame->Ip = FuncInst->CodePtr;  // 设置指令指针为函数代码起始位置
  Frame->PrevFrame = getCurFrame();  // 链接到前一个栈帧

  setCurFrame(Frame);  // 设置当前栈帧
#ifdef ZEN_ENABLE_DWASM
  runtime::Instance *Inst = getInstance();  // 获取实例对象
  uint32_t CurFuncStackCost =
      (FuncInst->NumParamCells + FuncInst->NumLocalCells) << 2;  // 计算当前函数栈开销
  Inst->updateStackCost(CurFuncStackCost);  // 更新总栈开销
  if (Inst->getStackCost() > PresetReservedStackSize) {  // 检查是否超过预设栈大小限制
    throw getError(ErrorCode::DWasmCallStackExceed);  // 抛出栈溢出异常
  }
#endif // ZEN_ENABLE_DWASM
  return Frame;  // 返回新分配的栈帧
}

void InterpreterExecContext::freeFrame(FunctionInstance *FuncInst,
                                       InterpFrame *Frame) {  // 释放函数调用完成后的栈帧
  uint8_t *NewTop = reinterpret_cast<uint8_t *>(Frame);  // 获取栈帧起始位置
  uint32_t LocalSize = FuncInst->NumLocalCells << 2;  // 计算局部变量占用空间
  NewTop -= LocalSize;  // 回退到局部变量起始位置

  ZEN_ASSERT(NewTop >= Stack->Bottom);  // 确保不会回退到栈底以下
  Stack->Top = NewTop;  // 恢复栈顶指针，释放栈帧及其所有内容

#ifdef ZEN_ENABLE_DWASM
  uint32_t CurFuncStackCost =
      (FuncInst->NumParamCells + FuncInst->NumLocalCells) << 2;  // 计算当前函数栈开销
  getInstance()->updateStackCost(-CurFuncStackCost);  // 减少栈开销计数
#endif // ZEN_ENABLE_DWASM
}

enum BinaryOperator {  // 二元操作符枚举，定义所有支持的二元运算类型
  BO_ADD,      // 加法运算
  BO_SUB,      // 减法运算
  BO_MUL,      // 乘法运算
  BO_DIV,      // 无符号除法
  BO_DIV_S,    // 有符号除法
  BO_EQ,       // 相等比较
  BO_NE,       // 不等比较
  BO_LT,       // 小于比较
  BO_GT,       // 大于比较
  BO_LE,       // 小于等于比较
  BO_GE,       // 大于等于比较
  BO_REM_S,    // 有符号取余
  BO_REM_U,    // 无符号取余
  BO_AND,      // 按位与
  BO_OR,       // 按位或
  BO_XOR,      // 按位异或
  BO_SHL,      // 左移
  BO_SHR,      // 右移
  BO_ROTL,     // 左旋转
  BO_ROTR,     // 右旋转
  BO_MIN,      // 最小值（浮点）
  BO_MAX,      // 最大值（浮点）
  BO_COPYSIGN, // 复制符号位（浮点）

  BC_CLZ,           // 计算前导零个数
  BC_CTZ,           // 计算尾随零个数
  BC_POP_COUNT_I32, // 计算32位整数中1的个数
  BC_POP_COUNT_I64, // 计算64位整数中1的个数

  BM_SQRT,     // 平方根
  BM_FLOOR,    // 向下取整
  BM_CEIL,     // 向上取整
  BM_TRUNC,    // 截断取整
  BM_NEAREST,  // 四舍五入
  BM_ABS,      // 绝对值
  BM_NEG_F32,  // 32位浮点数取负
  BM_NEG_F64   // 64位浮点数取负
};

template <typename T, typename std::enable_if<!std::is_floating_point<T>::value,
                                              int>::type = 0>
T CanonNaN(T Val) {  // 对于非浮点类型，直接返回原值（不需要NaN规范化）
  return Val;
}

template <typename T, typename std::enable_if<std::is_floating_point<T>::value,
                                              int>::type = 0>
T CanonNaN(T Val) {  // 对于浮点类型，将NaN规范化为标准quiet NaN
  if (std::isnan(Val)) {  // 检查是否为NaN
    return std::numeric_limits<float>::quiet_NaN();  // 返回标准quiet NaN
  }
  return Val;  // 非NaN值直接返回
}

template <typename T, BinaryOperator Op> struct BinaryOpHelper {  // 二元操作辅助模板类
public:
  T operator()(T LHS, T RHS);  // 执行指定的二元操作
};

template <typename T> struct BinaryOpHelper<T, BO_DIV_S> {  // 有符号除法特化
public:
  T operator()(T LHS, T RHS) {
    if constexpr (sizeof(T) == 8) {  // 64位整数
      if (LHS == (static_cast<int64_t>(0x1ULL << 63)) && RHS == -1) {  // 检查最小值除以-1的溢出情况
        throw getError(ErrorCode::IntegerOverflow);  // 抛出整数溢出异常
      }
    } else {  // 32位整数
      static_assert(sizeof(T) == 4);
      if (LHS == (static_cast<int32_t>(0x1U << 31)) && RHS == -1) {  // 检查最小值除以-1的溢出情况
        throw getError(ErrorCode::IntegerOverflow);  // 抛出整数溢出异常
      }
    }
    if (RHS == 0) {  // 检查除零错误
      throw getError(ErrorCode::IntegerDivByZero);  // 抛出除零异常
    }
    return LHS / RHS;  // 执行有符号除法
  }
};

template <typename T> struct BinaryOpHelper<T, BO_DIV> {  // 除法操作特化（支持整数和浮点）
public:
  T operator()(T LHS, T RHS) {
    if constexpr (!std::is_floating_point<T>::value) {  // 整数除法
      if (RHS == 0) {  // 检查除零错误
        throw getError(ErrorCode::IntegerDivByZero);  // 抛出除零异常
      }
      return LHS / RHS;  // 执行整数除法
    } else {  // 浮点除法
      // We need to know whether RHS/LHS is exactly 0, so we should not use
      // approximate judgments like num < 1e-6
      // 需要精确判断RHS/LHS是否为0，不能使用近似判断如num < 1e-6
      // 0.0 / 0.0 is NaN
      // 1e-10 / 0.0 is +∞
      // -1e-10 / 0.0 is -∞
      if (RHS == 0) {  // 浮点除零处理
        return std::isnan(LHS) || LHS == 0  // 0/0或NaN/0返回NaN
                   ? std::numeric_limits<T>::quiet_NaN()
                   : ((std::signbit(LHS) ^ std::signbit(RHS))  // 根据符号位确定无穷大的符号
                          ? -std::numeric_limits<T>::infinity()  // 负无穷大
                          : std::numeric_limits<T>::infinity());  // 正无穷大
      }
      return CanonNaN(LHS / RHS);  // 执行浮点除法并规范化NaN
    }
  }
};

template <typename T> struct BinaryOpHelper<T, BO_REM_S> {  // 有符号取余操作特化
public:
  T operator()(T LHS, T RHS) {
    if constexpr (sizeof(T) == 8) {  // 64位整数
      if (LHS == (static_cast<int64_t>(0x1ULL << 63)) && RHS == -1) {  // 最小值对-1取余
        return 0;  // 特殊情况：最小值%-1=0（避免溢出）
      }
    } else {  // 32位整数
      static_assert(sizeof(T) == 4);
      if (LHS == (static_cast<int32_t>(0x1U << 31)) && RHS == -1) {  // 最小值对-1取余
        return 0;  // 特殊情况：最小值%-1=0（避免溢出）
      }
    }

    if (RHS == 0) {  // 检查除零错误
      throw getError(ErrorCode::IntegerDivByZero);  // 抛出除零异常
    }

    return LHS % RHS;  // 执行有符号取余
  }
};

template <typename T> struct BinaryOpHelper<T, BO_REM_U> {  // 无符号取余操作特化
public:
  T operator()(T LHS, T RHS) {
    if (RHS == 0) {  // 检查除零错误
      throw getError(ErrorCode::IntegerDivByZero);  // 抛出除零异常
    }
    return LHS % RHS;  // 执行无符号取余
  }
};

template <typename T> struct BinaryOpHelper<T, BO_ROTL> {  // 左旋转操作特化
public:
  T operator()(T LHS, T RHS) {
    constexpr uint32_t TypeBitNum = sizeof(T) << 3;  // 计算类型的位数
    RHS = RHS % TypeBitNum;  // 旋转位数对类型位数取模
    constexpr uint32_t Mask = TypeBitNum - 1;  // 创建位掩码
    RHS &= Mask;  // 确保旋转位数在有效范围内
    return (LHS << RHS) | (LHS >> ((0 - RHS) & Mask));  // 执行左旋转：左移+右移组合
  }
};

template <typename T> struct BinaryOpHelper<T, BO_ROTR> {  // 右旋转操作特化
public:
  T operator()(T LHS, T RHS) {
    constexpr uint32_t TypeBitNum = sizeof(T) << 3;  // 计算类型的位数
    RHS = RHS % TypeBitNum;  // 旋转位数对类型位数取模
    constexpr uint32_t Mask = TypeBitNum - 1;  // 创建位掩码
    RHS &= Mask;  // 确保旋转位数在有效范围内
    return (LHS >> RHS) | (LHS << ((0 - RHS) & Mask));  // 执行右旋转：右移+左移组合
  }
};

template <typename T> struct BinaryOpHelper<T, BO_MIN> {  // 浮点最小值操作特化
public:
  T operator()(T LHS, T RHS) {
    if (std::isnan(LHS) || std::isnan(RHS)) {  // 如果任一操作数为NaN
      return std::numeric_limits<T>::quiet_NaN();  // 返回NaN
    }
    if (LHS == 0 && RHS == 0) {  // 处理+0和-0的情况
      return std::signbit(LHS) ? LHS : RHS;  // 返回负零（如果存在）
    }
    return std::min(LHS, RHS);  // 返回较小值
  }
};

template <typename T> struct BinaryOpHelper<T, BO_MAX> {  // 浮点最大值操作特化
public:
  T operator()(T LHS, T RHS) {
    if (std::isnan(LHS) || std::isnan(RHS)) {  // 如果任一操作数为NaN
      return std::numeric_limits<T>::quiet_NaN();  // 返回NaN
    }
    if (LHS == 0 && RHS == 0) {  // 处理+0和-0的情况
      return std::signbit(LHS) ? RHS : LHS;  // 返回正零（如果存在）
    }
    return std::max(LHS, RHS);  // 返回较大值
  }
};

template <typename T> struct BinaryOpHelper<T, BO_COPYSIGN> {  // 复制符号位操作特化
public:
  T operator()(T LHS, T RHS) {
    return std::signbit(RHS) ? -std::fabs(LHS) : std::fabs(LHS);  // 将RHS的符号位复制到LHS的绝对值
  }
};

#define DECL_BINOP_IMPL(Opr, Operation)                                        \
  template <typename T> struct BinaryOpHelper<T, BO_##Opr> {                   \
  public:                                                                      \
    T operator()(T LHS, T RHS) { return CanonNaN(LHS Operation RHS); }         \
  };  // 宏定义：为简单二元操作生成模板特化

DECL_BINOP_IMPL(ADD, +)  // 加法操作
DECL_BINOP_IMPL(SUB, -)  // 减法操作
DECL_BINOP_IMPL(MUL, *)  // 乘法操作
DECL_BINOP_IMPL(AND, &)  // 按位与操作
DECL_BINOP_IMPL(OR, |)   // 按位或操作
DECL_BINOP_IMPL(XOR, ^)  // 按位异或操作
DECL_BINOP_IMPL(SHL, <<) // 左移操作
DECL_BINOP_IMPL(SHR, >>) // 右移操作
#undef DECL_BINOP_IMPL

#define DECL_COMPARE_IMPL(Opr, Operation)                                      \
  template <typename T> struct BinaryOpHelper<T, BO_##Opr> {                   \
  public:                                                                      \
    int32_t operator()(T LHS, T RHS) { return LHS Operation RHS; }             \
  };  // 宏定义：为比较操作生成模板特化（返回int32_t）

DECL_COMPARE_IMPL(EQ, ==) // 相等比较
DECL_COMPARE_IMPL(NE, !=) // 不等比较
DECL_COMPARE_IMPL(LT, <)  // 小于比较
DECL_COMPARE_IMPL(GT, >)  // 大于比较
DECL_COMPARE_IMPL(LE, <=) // 小于等于比较
DECL_COMPARE_IMPL(GE, >=) // 大于等于比较
#undef DECL_COMPARE_IMPL

class BaseInterpreterImpl {  // 解释器核心实现类
private:
  InterpreterExecContext &Context;  // 解释器执行上下文引用

public:
  BaseInterpreterImpl(InterpreterExecContext &Context) : Context(Context) {}  // 构造函数
  void interpret();  // 主解释循环入口

private:
  struct CacheValue {  // 块地址缓存值结构
    const uint8_t *ElsePtr = nullptr;  // else指令地址
    const uint8_t *EndPtr = nullptr;   // end指令地址
  };
  std::unordered_map<const uint8_t *, CacheValue> BlockAddrCache;  // 块地址缓存映射表

  void findBlockAddr(const uint8_t *Start, const uint8_t *End,
                     const uint8_t *&ElseAddr, const uint8_t *&EndAddr) {  // 查找块结构的else和end地址
    auto It = BlockAddrCache.find(Start);  // 在缓存中查找
    if (It != BlockAddrCache.end()) {  // 如果缓存命中
      auto &CacheValue = It->second;  // 获取缓存值
      ElseAddr = CacheValue.ElsePtr;  // 设置else地址
      EndAddr = CacheValue.EndPtr;    // 设置end地址
      return;  // 直接返回，避免重复解析
    }

    const uint8_t *Ptr = Start;  // 当前解析指针
    int32_t BlockDepth = 1;      // 块嵌套深度计数器

    std::unordered_map<int32_t, const uint8_t *> BlockStartPtrs;  // 各层块的起始地址映射

    BlockStartPtrs[0] = Start;  // 记录当前块的起始地址

    while (Ptr < End) {  // 遍历字节码直到结束
      uint8_t Opcode = *Ptr++;  // 读取操作码
      switch (Opcode) {
      case UNREACHABLE:  // 不可达指令
      case NOP:          // 空操作指令
        break;           // 无需额外处理
      case BLOCK:        // 块开始
      case LOOP:         // 循环开始
      case IF:           // 条件分支开始
        // process type
        Ptr = skipBlockType(Ptr, End);  // 跳过块类型信息
        BlockStartPtrs[BlockDepth] = Ptr;  // 记录新块的起始地址
        BlockDepth++;  // 增加嵌套深度
        break;
      case ELSE: {  // else分支
        BlockAddrCache[BlockStartPtrs[BlockDepth - 1]].ElsePtr = Ptr - 1;  // 缓存else地址
        break;
      }
      case BR:     // 无条件分支
      case BR_IF:  // 条件分支
        Ptr = skipLEBNumber<int32_t>(Ptr, End);  // 跳过分支目标索引
        break;
      case BR_TABLE: {  // 分支表
        uint32_t NumTargets = 0;
        Ptr = readSafeLEBNumber(Ptr, NumTargets);  // 读取目标数量
        for (uint32_t I = 0; I <= NumTargets; ++I) {  // 跳过所有分支目标
          Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        }
        break;
      }
      case END: {  // 块结束
        BlockAddrCache[BlockStartPtrs[BlockDepth - 1]].EndPtr = Ptr - 1;  // 缓存end地址
        if (--BlockDepth == 0) {  // 如果回到顶层块
          auto &CacheValue = BlockAddrCache[BlockStartPtrs[0]];  // 获取顶层块缓存
          ElseAddr = CacheValue.ElsePtr;  // 设置else地址
          EndAddr = CacheValue.EndPtr;    // 设置end地址
          return;  // 解析完成，返回
        }
        break;
      }
      case DROP:
      case DROP_64:
      case SELECT:
      case SELECT_64:
        break;
      case GET_GLOBAL_64:
      case SET_GLOBAL_64: {
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        break;
      }
      case GET_LOCAL:
      case SET_LOCAL:
      case TEE_LOCAL:
      case GET_GLOBAL:
      case SET_GLOBAL:
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        break;
      case I32_CONST:
        Ptr = skipLEBNumber<int32_t>(Ptr, End);
        break;
      case I64_CONST:
        Ptr = skipLEBNumber<int64_t>(Ptr, End);
        break;
      case F32_CONST:
        Ptr += sizeof(float);
        break;
      case F64_CONST:
        Ptr += sizeof(double);
        break;
      case I32_EQZ:
      case I32_EQ:
      case I32_NE:
      case I32_LT_S:
      case I32_LT_U:
      case I32_GT_S:
      case I32_GT_U:
      case I32_LE_S:
      case I32_LE_U:
      case I32_GE_S:
      case I32_GE_U:
      case I64_EQZ:
      case I64_EQ:
      case I64_NE:
      case I64_LT_S:
      case I64_LT_U:
      case I64_GT_S:
      case I64_GT_U:
      case I64_LE_S:
      case I64_LE_U:
      case I64_GE_S:
      case I64_GE_U:
      case F32_EQ:
      case F32_NE:
      case F32_LT:
      case F32_GT:
      case F32_LE:
      case F32_GE:
      case F64_EQ:
      case F64_NE:
      case F64_LT:
      case F64_GT:
      case F64_LE:
      case F64_GE:
      case I32_CLZ:
      case I32_CTZ:
      case I32_POPCNT:
      case I32_ADD:
      case I32_SUB:
      case I32_MUL:
      case I32_DIV_S:
      case I32_DIV_U:
      case I32_REM_S:
      case I32_REM_U:
      case I32_AND:
      case I32_OR:
      case I32_XOR:
      case I32_SHL:
      case I32_SHR_S:
      case I32_SHR_U:
      case I32_ROTL:
      case I32_ROTR:
      case I64_CLZ:
      case I64_CTZ:
      case I64_POPCNT:
      case I64_ADD:
      case I64_SUB:
      case I64_MUL:
      case I64_DIV_S:
      case I64_DIV_U:
      case I64_REM_S:
      case I64_REM_U:
      case I64_AND:
      case I64_OR:
      case I64_XOR:
      case I64_SHL:
      case I64_SHR_S:
      case I64_SHR_U:
      case I64_ROTL:
      case I64_ROTR:
      case F32_ABS:
      case F32_NEG:
      case F32_CEIL:
      case F32_FLOOR:
      case F32_TRUNC:
      case F32_NEAREST:
      case F32_SQRT:
      case F32_ADD:
      case F32_SUB:
      case F32_MUL:
      case F32_DIV:
      case F32_MIN:
      case F32_MAX:
      case F32_COPYSIGN:
      case F64_ABS:
      case F64_NEG:
      case F64_CEIL:
      case F64_FLOOR:
      case F64_TRUNC:
      case F64_NEAREST:
      case F64_SQRT:
      case F64_ADD:
      case F64_SUB:
      case F64_MUL:
      case F64_DIV:
      case F64_MIN:
      case F64_MAX:
      case F64_COPYSIGN:
      case I32_WRAP_I64:
      case I32_TRUNC_S_F32:
      case I32_TRUNC_U_F32:
      case I32_TRUNC_S_F64:
      case I32_TRUNC_U_F64:
      case I64_EXTEND_S_I32:
      case I64_EXTEND_U_I32:
      case I64_TRUNC_S_F32:
      case I64_TRUNC_U_F32:
      case I64_TRUNC_S_F64:
      case I64_TRUNC_U_F64:
      case F32_CONVERT_S_I32:
      case F32_CONVERT_U_I32:
      case F32_CONVERT_S_I64:
      case F32_CONVERT_U_I64:
      case F32_DEMOTE_F64:
      case F64_CONVERT_S_I32:
      case F64_CONVERT_U_I32:
      case F64_CONVERT_S_I64:
      case F64_CONVERT_U_I64:
      case F64_PROMOTE_F32:
      case I32_REINTERPRET_F32:
      case I64_REINTERPRET_F64:
      case F32_REINTERPRET_I32:
      case F64_REINTERPRET_I64:
      case I32_EXTEND8_S:
      case I32_EXTEND16_S:
      case I64_EXTEND8_S:
      case I64_EXTEND16_S:
      case I64_EXTEND32_S:
        break;
      case I32_LOAD:
      case I64_LOAD:
      case F32_LOAD:
      case F64_LOAD:
      case I32_LOAD8_S:
      case I32_LOAD8_U:
      case I32_LOAD16_S:
      case I32_LOAD16_U:
      case I64_LOAD8_S:
      case I64_LOAD8_U:
      case I64_LOAD16_S:
      case I64_LOAD16_U:
      case I64_LOAD32_S:
      case I64_LOAD32_U:
      case I32_STORE:
      case I64_STORE:
      case F32_STORE:
      case F64_STORE:
      case I32_STORE8:
      case I32_STORE16:
      case I64_STORE8:
      case I64_STORE16:
      case I64_STORE32:
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        break;
      case MEMORY_SIZE:
      case MEMORY_GROW:
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        break;
      case RETURN:
        break;
      case CALL: {
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        break;
      }
      case CALL_INDIRECT: {
        Ptr = skipLEBNumber<uint32_t>(Ptr, End);
        Ptr++;
        break;
      }
      default:
        ZEN_LOG_ERROR("unimplemented opcode : {%d}", Opcode);
        ZEN_ASSERT_TODO();
        break;
      }
    }

    return;
  }

  void updateFrame(const uint8_t *&Ip, const uint8_t *&IpEnd,
                   InterpFrame *&Frame, uint32_t *&ValStackPtr,
                   BlockInfo *&ControlStackPtr, uint32_t *&LocalPtr,
                   FunctionInstance *&FuncInst, bool IsReturn);

  void syncFrame(const uint8_t *Ip, InterpFrame *&Frame, uint32_t *ValStackPtr,
                 BlockInfo *ControlStackPtr);

  void callFuncInst(FunctionInstance *FuncInstCallee,
                    InterpreterExecContext &Context, const uint8_t *&Ip,
                    const uint8_t *&IpEnd, InterpFrame *&Frame,
                    uint32_t *&ValStackPtr, BlockInfo *&ControlStackPtr,
                    uint32_t *&LocalPtr, FunctionInstance *&FuncInst);

  /*
   * 带溢出检查的算术运算实现
   * 功能：模板函数，实现带溢出检查的基本算术运算（加法、减法、乘法）

    支持的操作：

    有符号运算：BO_ADD（加法）、BO_SUB（减法）、BO_MUL（乘法）
    无符号运算：BO_ADD（加法）、BO_SUB（减法）、BO_MUL（乘法）
    关键特性：

    使用编译器内建函数（__builtin_add_overflow、__builtin_sub_overflow、__builtin_mul_overflow）进行溢出检测
            支持有符号和无符号两种运算模式
                溢出时抛出 IntegerOverflow 异常
    模板参数化支持不同的数据类型
   */
  // 带溢出检查的算术运算实现模板函数
  // 支持有符号和无符号的加法、减法、乘法运算，使用编译器内建函数检测溢出
  template <bool Sign, BinaryOperator Opr, typename SignedT, typename UnsignedT,
            typename WasmReturnType>
  WasmReturnType handleCheckedArithmeticImpl(WasmReturnType LHS,
                                             WasmReturnType RHS) {
    if constexpr (Sign && Opr == BinaryOperator::BO_ADD) {  // 有符号加法
      SignedT Result = 0;  // 初始化结果变量
      if (__builtin_add_overflow((SignedT)LHS, (SignedT)RHS, &Result)) {  // 使用编译器内建函数检查加法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>(Result);  // 返回转换后的结果
    } else if constexpr (Sign && Opr == BinaryOperator::BO_SUB) {  // 有符号减法
      SignedT Result = 0;  // 初始化结果变量
      if (__builtin_sub_overflow((SignedT)LHS, (SignedT)RHS, &Result)) {  // 使用编译器内建函数检查减法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>(Result);  // 返回转换后的结果
    } else if constexpr (Sign && Opr == BinaryOperator::BO_MUL) {  // 有符号乘法
      SignedT Result = 0;  // 初始化结果变量
      if (__builtin_mul_overflow((SignedT)LHS, (SignedT)RHS, &Result)) {  // 使用编译器内建函数检查乘法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>(Result);  // 返回转换后的结果
    } else if constexpr (!Sign && Opr == BinaryOperator::BO_ADD) {  // 无符号加法
      UnsignedT Result = 0;  // 初始化结果变量
      if (__builtin_add_overflow((UnsignedT)LHS, (UnsignedT)RHS, &Result)) {  // 使用编译器内建函数检查加法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>((UnsignedT)Result);  // 返回转换后的结果
    } else if constexpr (!Sign && Opr == BinaryOperator::BO_SUB) {  // 无符号减法
      UnsignedT Result = 0;  // 初始化结果变量
      if (__builtin_sub_overflow((UnsignedT)LHS, (UnsignedT)RHS, &Result)) {  // 使用编译器内建函数检查减法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>((UnsignedT)Result);  // 返回转换后的结果
    } else if constexpr (!Sign && Opr == BinaryOperator::BO_MUL) {  // 无符号乘法
      UnsignedT Result = 0;  // 初始化结果变量
      if (__builtin_mul_overflow((UnsignedT)LHS, (UnsignedT)RHS, &Result)) {  // 使用编译器内建函数检查乘法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<WasmReturnType>((UnsignedT)Result);  // 返回转换后的结果
    } else {
      // unreachable
      ZEN_ABORT();  // 不可达代码路径，程序终止
    }
  }

  // 128位整数算术运算实现函数
  // 将高64位和低64位组装成128位操作数，执行加法或减法运算
  // 支持有符号和无符号两种模式，成功时返回128位结果的高64位，溢出时抛出异常
  template <bool Sign, BinaryOperator Opr>
  int64_t handleCheckedI128ArithmeticImpl(int64_t LHSLo, int64_t LHSHi,
                                          int64_t RHSLo, int64_t RHSHi) {
    if constexpr (Sign && Opr == BinaryOperator::BO_ADD) {  // 有符号128位加法
      __int128_t LHS =
          (((__int128_t)LHSHi) << 64) + (__int128_t)((uint64_t)LHSLo);  // 组装左操作数：高64位+低64位
      __int128_t RHS =
          (((__int128_t)RHSHi) << 64) + (__int128_t)((uint64_t)RHSLo);  // 组装右操作数：高64位+低64位
      __int128_t Result = 0;  // 初始化128位结果
      if (__builtin_add_overflow(LHS, RHS, &Result)) {  // 检查128位加法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<int64_t>(Result >> 64);  // 返回结果的高64位
    } else if constexpr (Sign && Opr == BinaryOperator::BO_SUB) {  // 有符号128位减法
      __int128_t LHS =
          (((__int128_t)LHSHi) << 64) + (__int128_t)((uint64_t)LHSLo);  // 组装左操作数
      __int128_t RHS =
          (((__int128_t)RHSHi) << 64) + (__int128_t)((uint64_t)RHSLo);  // 组装右操作数
      __int128_t Result = 0;  // 初始化128位结果
      if (__builtin_sub_overflow(LHS, RHS, &Result)) {  // 检查128位减法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<int64_t>(Result >> 64);  // 返回结果的高64位
    } else if constexpr (!Sign && Opr == BinaryOperator::BO_ADD) {  // 无符号128位加法
      __uint128_t LHS =
          (((__uint128_t)((uint64_t)LHSHi)) << 64) + (uint64_t)LHSLo;  // 组装无符号左操作数
      __uint128_t RHS =
          (((__uint128_t)((uint64_t)RHSHi)) << 64) + (uint64_t)RHSLo;  // 组装无符号右操作数
      __uint128_t Result = 0;  // 初始化无符号128位结果
      if (__builtin_add_overflow(LHS, RHS, &Result)) {  // 检查无符号128位加法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<int64_t>(Result >> 64);  // 返回结果的高64位
    } else if constexpr (!Sign && Opr == BinaryOperator::BO_SUB) {  // 无符号128位减法
      __uint128_t LHS =
          (((__uint128_t)((uint64_t)LHSHi)) << 64) + (uint64_t)LHSLo;  // 组装无符号左操作数
      __uint128_t RHS =
          (((__uint128_t)((uint64_t)RHSHi)) << 64) + (uint64_t)RHSLo;  // 组装无符号右操作数
      __uint128_t Result = 0;  // 初始化无符号128位结果
      if (__builtin_sub_overflow(LHS, RHS, &Result)) {  // 检查无符号128位减法溢出
        throw getError(ErrorCode::IntegerOverflow);  // 溢出时抛出异常
      }
      return static_cast<int64_t>(Result >> 64);  // 返回结果的高64位
    } else {
      // unreachable
      ZEN_ABORT();  // 不可达代码路径，程序终止
    }
  }

  // 算术运算调度函数
  // 根据WASM类型调度相应的算术运算实现，支持I8/I16/I32/I64类型
  // 从值栈弹出操作数，调用对应类型的算术运算实现，将结果推入值栈
  template <bool Sign, WASMType Type, BinaryOperator Opr>
  void handleCheckedArithmetic() {
    InterpFrame *Frame = Context.getCurFrame();  // 获取当前栈帧
    uint32_t *&ValStackPtr = Frame->ValueStackPtr; // must be reference  // 获取值栈指针（必须是引用）
    if constexpr (Type == WASMType::I8) {  // 8位整数类型
      auto RHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出右操作数
      auto LHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出左操作数
      auto Res =
          handleCheckedArithmeticImpl<Sign, Opr, int8_t, uint8_t, int32_t>(LHS,
                                                                           RHS);  // 执行8位算术运算
      Frame->valuePush<int32_t>(ValStackPtr, Res);  // 将结果推入值栈
    } else if constexpr (Type == WASMType::I16) {  // 16位整数类型
      auto RHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出右操作数
      auto LHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出左操作数
      auto Res =
          handleCheckedArithmeticImpl<Sign, Opr, int16_t, uint16_t, int32_t>(
              LHS, RHS);  // 执行16位算术运算
      Frame->valuePush<int32_t>(ValStackPtr, Res);  // 将结果推入值栈
    } else if constexpr (Type == WASMType::I32) {  // 32位整数类型
      auto RHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出右操作数
      auto LHS = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出左操作数
      auto Res =
          handleCheckedArithmeticImpl<Sign, Opr, int32_t, uint32_t, int32_t>(
              LHS, RHS);  // 执行32位算术运算
      Frame->valuePush<int32_t>(ValStackPtr, Res);  // 将结果推入值栈
    } else if constexpr (Type == WASMType::I64) {  // 64位整数类型
      auto RHS = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出右操作数
      auto LHS = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出左操作数
      auto Res =
          handleCheckedArithmeticImpl<Sign, Opr, int64_t, uint64_t, int64_t>(
              LHS, RHS);  // 执行64位算术运算
      Frame->valuePush<int64_t>(ValStackPtr, Res);  // 将结果推入值栈
    } else {
      ZEN_ABORT();  // 不支持的类型，程序终止
    }
  }

  // 128位整数运算调度函数
  // 专门处理128位整数运算，按顺序弹出四个64位部分组成两个128位操作数
  template <bool Sign, BinaryOperator Opr> void handleCheckedI128Arithmetic() {
    InterpFrame *Frame = Context.getCurFrame();  // 获取当前栈帧
    uint32_t *&ValStackPtr = Frame->ValueStackPtr; // must use reference  // 获取值栈指针（必须使用引用）
    auto RHSHi = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出右操作数高64位
    auto RHSLo = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出右操作数低64位
    auto LHSHi = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出左操作数高64位
    auto LHSLo = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出左操作数低64位
    auto Res =
        handleCheckedI128ArithmeticImpl<Sign, Opr>(LHSLo, LHSHi, RHSLo, RHSHi);  // 执行128位算术运算
    Frame->valuePush<int64_t>(ValStackPtr, Res);  // 将结果推入值栈
  }

  // 数学函数操作模板
  // 实现各种数学函数：平方根、取整、绝对值、浮点数取负等
  template <typename T, BinaryOperator Op>
  void binaryOpMath(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    T Val = Frame->valuePop<T>(ValStackPtr);  // 从值栈弹出操作数
    switch (Op) {  // 根据操作类型执行相应的数学函数
    case BM_SQRT:  // 平方根
      Frame->valuePush<T>(ValStackPtr, CanonNaN(std::sqrt(Val)));  // 计算平方根并推入栈
      break;
    case BM_FLOOR:  // 向下取整
      Frame->valuePush<T>(ValStackPtr, CanonNaN(std::floor(Val)));  // 向下取整并推入栈
      break;
    case BM_CEIL:  // 向上取整
      Frame->valuePush<T>(ValStackPtr, CanonNaN(std::ceil(Val)));  // 向上取整并推入栈
      break;
    case BM_TRUNC:  // 截断取整
      Frame->valuePush<T>(ValStackPtr, CanonNaN(std::trunc(Val)));  // 截断取整并推入栈
      break;
    case BM_NEAREST:  // 四舍五入
      Frame->valuePush<T>(ValStackPtr, CanonNaN(std::rint(Val)));  // 四舍五入并推入栈
      break;
    case BM_ABS:  // 绝对值
      Frame->valuePush<T>(ValStackPtr, std::fabs(Val));  // 计算绝对值并推入栈
      break;
    case BM_NEG_F32: {  // 32位浮点数取负
      uint32_t U32;
      std::memcpy(&U32, &Val, sizeof(uint32_t));  // 将浮点数按位复制到整数
      uint32_t SignBit = U32 & (((uint32_t)1) << 31);  // 提取符号位
      if (SignBit) {  // 如果原来是负数
        Frame->valuePush<uint32_t>(ValStackPtr, U32 & ~((uint32_t)1 << 31));  // 清除符号位（变为正数）
      } else {  // 如果原来是正数
        Frame->valuePush<uint32_t>(ValStackPtr, U32 | ((uint32_t)1 << 31));  // 设置符号位（变为负数）
      }
      break;
    }
    case BM_NEG_F64: {  // 64位浮点数取负
      uint64_t U64;
      std::memcpy(&U64, &Val, sizeof(uint64_t));  // 将浮点数按位复制到整数
      uint64_t SignBit64 = U64 & (((uint64_t)1) << 63);  // 提取符号位
      if (SignBit64) {  // 如果原来是负数
        Frame->valuePush<uint64_t>(ValStackPtr, U64 & ~((uint64_t)1 << 63));  // 清除符号位（变为正数）
      } else {  // 如果原来是正数
        Frame->valuePush<uint64_t>(ValStackPtr, U64 | ((uint64_t)1 << 63));  // 设置符号位（变为负数）
      }
      break;
    }
    }
    return;  // 函数返回
  }

  // 类型转换操作模板
  // 在不同数据类型之间进行简单的静态类型转换
  template <typename DstType, typename SrcType>
  void binaryOpCV(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    SrcType Val = Frame->valuePop<DstType>(ValStackPtr);  // 从值栈弹出源类型值
    Frame->valuePush<DstType>(ValStackPtr, Val);  // 将转换后的值推入值栈
    return;
  }

  // 零值检测操作模板
  // 检测值是否为零，返回布尔结果的整数表示
  template <typename T>
  void binaryOpNEZ(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    T Val = Frame->valuePop<T>(ValStackPtr);  // 从值栈弹出值
    Frame->valuePush<int32_t>(ValStackPtr, Val == 0);  // 检查是否为零并推入结果
  }

  // 位计数操作模板
  // 实现各种位计数操作：前导零、尾随零、位计数等
  template <typename T, BinaryOperator Op>
  void binaryOpCount(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    T Val = Frame->valuePop<T>(ValStackPtr);  // 从值栈弹出值
    uint32_t Num = 0;  // 初始化计数器
    uint32_t TypeBitNum = sizeof(T) << 3;  // 计算类型的位数
    // #if defined(__GNUC__)
    //         switch (op) {
    //             case BC_CLZ:
    //                 num = Val == 0             ? type_bit_num
    //                       : type_bit_num == 32 ? __builtin_clz(Val)
    //                                            : __builtin_clzl(Val);
    //                 break;
    //             case BC_CTZ:
    //                 num = Val == 0             ? type_bit_num
    //                       : type_bit_num == 32 ? __builtin_ctz(Val)
    //                                            : __builtin_ctzl(Val);
    //                 break;
    //             case BC_POP_COUNT_I32:
    //                 num = __builtin_popcount(Val);
    //                 break;
    //             case BC_POP_COUNT_I64:
    //                 num = __builtin_popcountl(Val);
    //                 break;
    //             default:
    //                 ZEN_ABORT();
    //         }
    //         frame->valuePush<T>(StackPtr, num);
    // #else
    switch (Op) {  // 根据操作类型执行相应的位计数
    case BC_CLZ: {  // 计算前导零个数
      std::bitset<64> ValBits(Val);  // 将值转换为位集
      if (ValBits.none()) {  // 如果所有位都是0
        Num = TypeBitNum;  // 前导零个数等于总位数
      } else {
        uint32_t Idx = (TypeBitNum - 1);  // 从最高位开始
        while (!ValBits.test(Idx)) {  // 计算前导零个数
          Num++;
          Idx--;
        }
      }
      Frame->valuePush<T>(ValStackPtr, Num);  // 推入结果
      break;
    }
    case BC_CTZ: {  // 计算尾随零个数
      std::bitset<64> ValBits(Val);  // 将值转换为位集
      if (ValBits.none()) {  // 如果所有位都是0
        Num = TypeBitNum;  // 尾随零个数等于总位数
      } else {
        uint32_t Idx = 0;  // 从最低位开始
        while (!ValBits.test(Idx) && Idx <= TypeBitNum) {  // 计算尾随零个数
          Num++;
          Idx++;
        }
      }
      Frame->valuePush<T>(ValStackPtr, Num);  // 推入结果
      break;
    }
    case BC_POP_COUNT_I32:  // 计算32位整数中1的个数
      Frame->valuePush<T>(ValStackPtr, std::bitset<32>(Val).count());  // 使用bitset计算1的个数
      break;
    case BC_POP_COUNT_I64:  // 计算64位整数中1的个数
      Frame->valuePush<T>(ValStackPtr, std::bitset<64>(Val).count());  // 使用bitset计算1的个数
      break;
    default:
      ZEN_ABORT();  // 不支持的操作类型
    }
    // #endif
  }

  // 选择操作模板（三元条件运算符）
  // 根据条件值选择两个候选值中的一个，类似于 condition ? value1 : value2
  template <typename T>
  void selectOp(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    uint32_t Cond = Frame->valuePop<uint32_t>(ValStackPtr);  // 弹出条件值
    T V1 = Frame->valuePop<T>(ValStackPtr);  // 弹出第一个候选值
    T V2 = Frame->valuePop<T>(ValStackPtr);  // 弹出第二个候选值
    if (Cond) {  // 如果条件为真
      Frame->valuePush<T>(ValStackPtr, V2);  // 推入第二个值
    } else {  // 如果条件为假
      Frame->valuePush<T>(ValStackPtr, V1);  // 推入第一个值
    }
  }

  // 通用二元操作模板
  // 使用BinaryOpHelper执行各种二元运算（算术、逻辑、比较等）
  template <typename T, BinaryOperator Op>
  void binaryOp(InterpFrame *Frame, uint32_t *&ValStackPtr) {
    T RHS = Frame->valuePop<T>(ValStackPtr);  // 弹出右操作数
    T LHS = Frame->valuePop<T>(ValStackPtr);  // 弹出左操作数

    auto Ret = BinaryOpHelper<T, Op>()(LHS, RHS);  // 使用二元操作辅助类执行运算
    Frame->valuePush<decltype(Ret)>(ValStackPtr, Ret);  // 将结果推入值栈
  }

  template <typename SrcType, typename DestType>
  void storeOp(MemoryInstance &Memory, const uint8_t *&Ip, const uint8_t *IpEnd,
               InterpFrame *Frame, uint32_t *&ValStackPtr,
               uint64_t LinearMemSize) {  // 内存存储操作模板函数
    uint32_t Align, Offset;  // 对齐和偏移参数
    Ip = readSafeLEBNumber(Ip, Align);    // 读取对齐参数（实际未使用）
    Ip = readSafeLEBNumber(Ip, Offset);   // 读取内存偏移量
    SrcType Val = Frame->valuePop<SrcType>(ValStackPtr);      // 从值栈弹出要存储的值
    uint32_t Addr = Frame->valuePop<uint32_t>(ValStackPtr);   // 从值栈弹出内存地址
    if ((uint64_t)Offset + sizeof(DestType) + Addr > LinearMemSize) {  // 检查内存边界
      throw getError(ErrorCode::OutOfBoundsMemory);  // 抛出内存越界异常
    }
    uint8_t *Start = Memory.MemBase + Offset + Addr;  // 计算实际内存地址
#ifdef ZEN_ENABLE_DEBUG_INTERP
    ZEN_LOG_DEBUG("StoreOp, addr: %d, offset: %d, value: %llu", Addr, Offset,
                  Val);  // 调试日志
#endif
    *(DestType *)Start = Val;  // 将值存储到内存中
  }

  template <typename DestType, typename SrcType>
  void loadOp(MemoryInstance &Memory, const uint8_t *&Ip, const uint8_t *IpEnd,
              InterpFrame *Frame, uint32_t *&ValStackPtr,
              uint64_t LinearMemSize) {  // 内存加载操作模板函数
    uint32_t Align, Offset;  // 对齐和偏移参数
    Ip = readSafeLEBNumber(Ip, Align);    // 读取对齐参数（实际未使用）
    Ip = readSafeLEBNumber(Ip, Offset);   // 读取内存偏移量
    uint32_t Addr = Frame->valuePop<uint32_t>(ValStackPtr);   // 从值栈弹出内存地址
    if ((uint64_t)Offset + sizeof(SrcType) + Addr > LinearMemSize) {  // 检查内存边界
      throw getError(ErrorCode::OutOfBoundsMemory);  // 抛出内存越界异常
    }
    uint8_t *Start = Memory.MemBase + Offset + Addr;  // 计算实际内存地址
#ifdef ZEN_ENABLE_DEBUG_INTERP
    ZEN_LOG_DEBUG("LoadOp, addr: %d, offset: %d, value: %llu", Addr, Offset,
                  *(SrcType *)Start);  // 调试日志
#endif
    Frame->valuePush<DestType>(ValStackPtr, *(SrcType *)Start);  // 从内存加载值并推入值栈
  }

  template <typename TargetType, typename SrcType, bool IsSigned>
  void truncate(InterpFrame *Frame, uint32_t *&ValStackPtr) {  // 浮点数截断为整数的模板函数
    static_assert(sizeof(TargetType) == 4 || sizeof(TargetType) == 8);  // 确保目标类型为32或64位
    auto Src = Frame->valuePop<SrcType>(ValStackPtr);  // 从值栈弹出源浮点数
    if (std::isnan(Src)) {  // 检查是否为NaN
      throw getError(ErrorCode::InvalidConversionToInteger);  // NaN无法转换为整数
    }
    auto Min = FloatAttr<SrcType>::template toIntMin<TargetType, IsSigned>();  // 获取转换范围最小值

    auto Max = FloatAttr<SrcType>::template toIntMax<TargetType, IsSigned>();  // 获取转换范围最大值
    if (Src <= Min || Src >= Max) {  // 检查是否超出转换范围
      throw getError(ErrorCode::IntegerOverflow);  // 抛出整数溢出异常
    }

    if (IsSigned) {  // 有符号转换
      Frame->valuePush<TargetType>(
          ValStackPtr, static_cast<TargetType>(static_cast<int64_t>(Src)));  // 转换为有符号整数
    } else {  // 无符号转换
      Frame->valuePush<TargetType>(
          ValStackPtr, static_cast<TargetType>(static_cast<uint64_t>(Src)));  // 转换为无符号整数
    }
  }

  template <typename TargetType, typename SrcType>
  void convert(InterpFrame *Frame, uint32_t *&ValStackPtr) {  // 类型转换模板函数
    Frame->valuePush<TargetType>(
        ValStackPtr,
        static_cast<TargetType>(Frame->valuePop<SrcType>(ValStackPtr)));  // 直接类型转换
  }
};

void BaseInterpreterImpl::updateFrame(
    const uint8_t *&Ip, const uint8_t *&IpEnd, InterpFrame *&Frame,
    uint32_t *&ValStackPtr, BlockInfo *&ControlStackPtr, uint32_t *&LocalPtr,
    FunctionInstance *&FuncInst, bool IsReturn) {  // 更新栈帧状态到局部变量
  // update frame
  Ip = Frame->Ip;  // 更新指令指针
  IpEnd = Frame->FuncInst->CodePtr + Frame->FuncInst->CodeSize;  // 更新指令结束地址
  ValStackPtr = Frame->ValueStackPtr;  // 更新值栈指针
  if (IsReturn) {  // 如果是函数返回
    ValStackPtr -= FuncInst->NumParamCells;  // 移除参数占用的栈空间
    ValStackPtr += FuncInst->NumReturnCells; // 为返回值预留栈空间
  }
  ControlStackPtr = Frame->CtrlStackPtr;  // 更新控制栈指针

  LocalPtr = (uint32_t *)Frame->LocalPtr;  // 更新局部变量指针
  FuncInst = Frame->FuncInst;  // 更新函数实例指针
}

void BaseInterpreterImpl::syncFrame(const uint8_t *Ip, InterpFrame *&Frame,
                                    uint32_t *ValStackPtr,
                                    BlockInfo *ControlStackPtr) {  // 同步局部变量状态到栈帧
  Frame->Ip = Ip;  // 同步指令指针
  Frame->ValueStackPtr = ValStackPtr;  // 同步值栈指针
  Frame->CtrlStackPtr = ControlStackPtr;  // 同步控制栈指针
}

void BaseInterpreterImpl::callFuncInst(
    FunctionInstance *Callee, InterpreterExecContext &Context,
    const uint8_t *&Ip, const uint8_t *&IpEnd, InterpFrame *&Frame,
    uint32_t *&ValStackPtr, BlockInfo *&ControlStackPtr, uint32_t *&LocalPtr,
    FunctionInstance *&FuncInst) {  // 函数调用处理（支持Native和ByteCode函数）

  ZEN_ASSERT(Callee != nullptr);  // 确保被调用函数不为空
  if (Callee->Kind == FunctionKind::Native) {  // 处理Native函数调用（宿主函数）
    // Prepare slots to pass arguments
    // 准备参数槽位以传递参数
    int32_t ParamCount = Callee->NumParams;  // 获取参数数量
    WASMType *ParamTypes = Callee->getParamTypes();  // 获取参数类型数组
    std::vector<TypedValue> Args(ParamCount);  // 创建参数向量

    for (int32_t I = ParamCount - 1; I >= 0; --I) {  // 逆序弹出参数（栈是LIFO）
      WASMType Type = ParamTypes[I];  // 获取参数类型
      Args[I].Type = Type;  // 设置参数类型
      UntypedValue &Value = Args[I].Value;  // 获取参数值引用
      switch (Type) {  // 根据类型从值栈弹出参数
      case WASMType::I32:
        Value.I32 = Frame->valuePop<int32_t>(ValStackPtr);  // 弹出32位整数
        break;
      case WASMType::I64:
        Value.I64 = Frame->valuePop<int64_t>(ValStackPtr);  // 弹出64位整数
        break;
      case WASMType::F32:
        Value.F32 = Frame->valuePop<float>(ValStackPtr);    // 弹出32位浮点数
        break;
      case WASMType::F64:
        Value.F64 = Frame->valuePop<double>(ValStackPtr);   // 弹出64位浮点数
        break;
      default:
        ZEN_ASSERT_TODO();  // 未支持的类型
      }
    }

    // Prepare slots to receive the return values.
    size_t ReturnCount = Callee->NumReturns;
    std::vector<TypedValue> Result(ReturnCount);
    for (size_t I = 0; I < ReturnCount; ++I) {
      Result[I].Type = Callee->ReturnTypes[I];
    }

    Instance *Instance = Context.getInstance();
#ifdef ZEN_ENABLE_DWASM
    if (Instance->getStackCost() >= PresetReservedStackSize) {
      // check call stack depth between hostapi call
      throw getError(ErrorCode::DWasmCallStackExceed);
    }
    Instance->setInHostAPI(true);
#endif // ZEN_ENABLE_DWASM

    entrypoint::callNativeGeneral(
        Instance, GenericFunctionPointer(Callee->CodePtr), Args, Result,
        Instance->getRuntime()->getMemAllocator(), true);

#ifdef ZEN_ENABLE_DWASM
    Instance->setInHostAPI(false);
#endif // ZEN_ENABLE_DWASM

    const Error &Err = Instance->getError();
    if (!Err.isEmpty()) {
      throw Err;
    }

    // Extract and push the return values to the stack
    for (size_t I = 0; I < ReturnCount; I++) {
      UntypedValue &Value = Result[I].Value;
      switch (Result[I].Type) {
      case WASMType::I32:
        Frame->valuePush<int32_t>(ValStackPtr, Value.I32);
        break;
      case WASMType::I64:
        Frame->valuePush<int64_t>(ValStackPtr, Value.I64);
        break;
      case WASMType::F32:
        Frame->valuePush<float>(ValStackPtr, Value.F32);
        break;
      case WASMType::F64:
        Frame->valuePush<double>(ValStackPtr, Value.F64);
        break;
      default:
        ZEN_ASSERT_TODO();
      }
    }
  } else if (Callee->Kind == FunctionKind::ByteCode) {

    // sync frames
    syncFrame(Ip, Frame, ValStackPtr, ControlStackPtr);

    Frame = Context.allocFrame((FunctionInstance *)Callee,
                               ValStackPtr - Callee->NumParamCells);
    if (Frame == nullptr) {
      throw getError(ErrorCode::CallStackExhausted);
    }
    // update frame
    updateFrame(Ip, IpEnd, Frame, ValStackPtr, ControlStackPtr, LocalPtr,
                FuncInst, false);

    // init local vars
    std::memset(LocalPtr + FuncInst->NumParamCells, 0,
                ((uint32_t)FuncInst->NumLocalCells) << 2);

    Frame->blockPush(ControlStackPtr, IpEnd - 1, ValStackPtr,
                     FuncInst->NumReturnCells, LABEL_FUNCTION);
  } else {
    ZEN_ASSERT_TODO();
  }
}

void BaseInterpreterImpl::interpret() {  // 主解释循环入口函数
#define DIRECT_DISPATCH 0  // 直接分发模式开关（当前未启用）
#if !DIRECT_DISPATCH
#define SWITCH(Ip) switch (Opcode = *Ip++)  // 标准switch分发模式
#define CASE(Op) case Op                     // case标签宏
#define DEFAULT default                      // default标签宏
#ifdef ZEN_ENABLE_DEBUG_INTERP
#define BREAK                                                                  \
  ZEN_LOG_DEBUG("opcode: %s", getOpcodeString(Opcode));                        \
  break  // 调试模式下的break宏（包含日志输出）
#else
#define BREAK break  // 普通模式下的break宏
#endif // ZEN_ENABLE_DEBUG_INTERP
#else  // TODO
#define SWITCH(Ip) switch (Opcode = *Ip++)  // 直接分发模式（待实现）
#define CASE(Op) case Op
#define DEFAULT default
#define BREAK break
#endif
  InterpFrame *Frame = Context.getCurFrame();  // 获取当前栈帧
  ZEN_ASSERT(Frame != nullptr);  // 确保栈帧有效
  const uint8_t *Ip = Frame->Ip;  // 获取指令指针
  const uint8_t *IpEnd = Ip + Frame->FuncInst->CodeSize;  // 计算指令结束地址
  uint32_t *ValStackPtr = Frame->ValueStackPtr;  // 获取值栈指针
  BlockInfo *ControlStackPtr = Frame->CtrlStackPtr;  // 获取控制栈指针
  uint32_t *LocalPtr = (uint32_t *)Frame->LocalPtr;  // 获取局部变量指针
  FunctionInstance *FuncInst = Frame->FuncInst;  // 获取函数实例
  Instance *ModInst = Context.getInstance();  // 获取模块实例
  const Module *Mod = ModInst->getModule();  // 获取模块定义
  MemoryInstance *Memory = nullptr;  // 内存实例指针
  uint64_t LinearMemSize = 0;  // 线性内存大小
  if (ModInst->hasMemory()) {  // 如果模块有内存
    Memory = &(ModInst->getDefaultMemoryInst());  // 获取默认内存实例
    LinearMemSize = Memory->MemSize;  // 获取内存大小
  }

  WASMType LocalType;  // 局部变量类型
  uint32_t LocalOffset, LocalIdx, FuncIdx, GlobalIdx, Cond, Depth;  // 各种索引和临时变量
  const uint8_t *ElseAddr = nullptr;  // else分支地址
  const uint8_t *EndAddr = nullptr;   // end指令地址
  uint8_t Opcode;  // 当前操作码

  Frame->blockPush(ControlStackPtr, IpEnd - 1, ValStackPtr,
                   FuncInst->NumReturnCells, LABEL_FUNCTION);  // 为函数创建顶层控制块

  // process starting imported function
  // 处理起始的导入函数
  if (FuncInst->Kind == FunctionKind::Native) {  // 如果是Native函数
    callFuncInst(FuncInst, Context, Ip, IpEnd, Frame, ValStackPtr,
                 ControlStackPtr, LocalPtr,
                 FuncInst); // the last arg is useless  // 最后一个参数无用
    return;  // Native函数执行完毕直接返回
  }

  while (Ip < IpEnd) {  // 主解释循环：逐条执行指令直到函数结束
    SWITCH(Ip) {  // 根据操作码分发到对应处理逻辑
      CASE(UNREACHABLE) : { throw getError(ErrorCode::Unreachable); }  // 不可达指令：抛出异常
      CASE(NOP) : { BREAK; }  // 空操作指令：什么都不做
      CASE(SELECT) : {
        selectOp<int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(SELECT_64) : {
        selectOp<int64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(BLOCK) : {
        uint32_t CellNum = getWASMTypeCellNumFromOpcode(*Ip++);

        findBlockAddr(Ip, IpEnd, ElseAddr, EndAddr);
        Frame->blockPush(ControlStackPtr, EndAddr, ValStackPtr, CellNum,
                         LABEL_BLOCK);
        BREAK;
      }
      CASE(LOOP) : {
        uint32_t CellNum = getWASMTypeCellNumFromOpcode(*Ip++);
        Frame->blockPush(ControlStackPtr, Ip, ValStackPtr, CellNum, LABEL_LOOP);
        BREAK;
      }
      CASE(BR) : {
        Ip = readSafeLEBNumber(Ip, Depth);
        Frame->blockPop(ControlStackPtr, ValStackPtr, Ip, Depth);
        BREAK;
      }
      CASE(BR_IF) : {
        Ip = readSafeLEBNumber(Ip, Depth);
        Cond = Frame->valuePop<int32_t>(ValStackPtr);
        if (Cond) {
          Frame->blockPop(ControlStackPtr, ValStackPtr, Ip, Depth);
        }
        BREAK;
      }
      CASE(BR_TABLE) : {
        uint32_t Count;
        Ip = readSafeLEBNumber(Ip, Count);
        uint32_t LabelIdx =
            std::min(Count, Frame->valuePop<uint32_t>(ValStackPtr));
        for (uint32_t I = 0; I < LabelIdx; I++) {
          Ip = skipLEBNumber<uint8_t>(Ip, IpEnd);
        }
        Ip = readSafeLEBNumber(Ip, Depth);
        Frame->blockPop(ControlStackPtr, ValStackPtr, Ip, Depth);
        BREAK;
      }
      CASE(DROP) : {
        Frame->valuePop<int32_t>(ValStackPtr);
        BREAK;
      }
      CASE(DROP_64) : {
        Frame->valuePop<int64_t>(ValStackPtr);
        BREAK;
      }
      CASE(IF) : {
        uint32_t CellNum = getWASMTypeCellNumFromOpcode(*Ip++);

        Cond = Frame->valuePop<int32_t>(ValStackPtr);
        findBlockAddr(Ip, IpEnd, ElseAddr, EndAddr);
        if (Cond) {
          Frame->blockPush(ControlStackPtr, EndAddr, ValStackPtr, CellNum,
                           LABEL_IF);
        } else {
          if (ElseAddr == nullptr) {
            Ip = EndAddr + 1;
          } else {
            Frame->blockPush(ControlStackPtr, EndAddr, ValStackPtr, CellNum,
                             LABEL_IF);
            Ip = ElseAddr + 1;
          }
        }
        BREAK;
      }
      CASE(ELSE) : {
        Ip = (ControlStackPtr - 1)->TargetAddr;
        BREAK;
      }
      CASE(GET_GLOBAL_64) : {
        Ip = readSafeLEBNumber(Ip, GlobalIdx);
        uint8_t *GlobalAddr = ModInst->getGlobalAddr(GlobalIdx);
        Frame->valuePush<int64_t>(ValStackPtr, *(int64_t *)GlobalAddr);
        BREAK;
      }
      CASE(SET_GLOBAL_64) : {
        Ip = readSafeLEBNumber(Ip, GlobalIdx);
        uint8_t *GlobalAddr = ModInst->getGlobalAddr(GlobalIdx);
        *(int64_t *)GlobalAddr = Frame->valuePop<int64_t>(ValStackPtr);
        BREAK;
      }
      CASE(GET_LOCAL) : {
        Ip = readSafeLEBNumber(Ip, LocalIdx);
        LocalType = FuncInst->getLocalType(LocalIdx);
        LocalOffset = FuncInst->getLocalOffset(LocalIdx);

        switch (LocalType) {
        case WASMType::F32:
        case WASMType::I32:
          Frame->valuePush<int32_t>(
              ValStackPtr,
              Frame->valueGet<int32_t>(ValStackPtr, LocalPtr + LocalOffset));
          break;
        case WASMType::F64:
        case WASMType::I64:
          Frame->valuePush<int64_t>(
              ValStackPtr,
              Frame->valueGet<int64_t>(ValStackPtr, LocalPtr + LocalOffset));
          break;
        default:
          ZEN_ASSERT_TODO();
          break;
        }
        BREAK;
      }
      CASE(SET_LOCAL) : {
        Ip = readSafeLEBNumber(Ip, LocalIdx);
        LocalType = FuncInst->getLocalType(LocalIdx);
        LocalOffset = FuncInst->getLocalOffset(LocalIdx);

        switch (LocalType) {
        case WASMType::F32:
        case WASMType::I32:
          Frame->valueSet<int32_t>(ValStackPtr, LocalPtr + LocalOffset,
                                   Frame->valuePop<int32_t>(ValStackPtr));
          break;
        case WASMType::F64:
        case WASMType::I64:
          Frame->valueSet<int64_t>(ValStackPtr, LocalPtr + LocalOffset,
                                   Frame->valuePop<int64_t>(ValStackPtr));
          break;
        default:
          ZEN_ASSERT_TODO();
        }
        BREAK;
      }
      CASE(TEE_LOCAL) : {
        Ip = readSafeLEBNumber(Ip, LocalIdx);
        LocalType = FuncInst->getLocalType(LocalIdx);
        LocalOffset = FuncInst->getLocalOffset(LocalIdx);

        switch (LocalType) {
        case WASMType::F32:
        case WASMType::I32:
          Frame->valueSet<int32_t>(ValStackPtr, LocalPtr + LocalOffset,
                                   Frame->valuePeek<int32_t>(ValStackPtr));
          break;
        case WASMType::F64:
        case WASMType::I64:
          Frame->valueSet<int64_t>(ValStackPtr, LocalPtr + LocalOffset,
                                   Frame->valuePeek<int64_t>(ValStackPtr));
          break;
        default:
          ZEN_ASSERT_TODO();
        }
        BREAK;
      }
      CASE(GET_GLOBAL) : {
        Ip = readSafeLEBNumber(Ip, GlobalIdx);
        uint8_t *GlobalAddr = ModInst->getGlobalAddr(GlobalIdx);
        WASMType GlobalType = ModInst->getGlobalType(GlobalIdx);
        switch (GlobalType) {
        case WASMType::I32:
        case WASMType::F32:
          Frame->valuePush<int32_t>(ValStackPtr, *(int32_t *)GlobalAddr);
          break;
        case WASMType::I64:
        case WASMType::F64:
          Frame->valuePush<int64_t>(ValStackPtr, *(int64_t *)GlobalAddr);
          break;
        default:
          ZEN_ASSERT_TODO();
        }
        BREAK;
      }
      CASE(SET_GLOBAL) : {
        Ip = readSafeLEBNumber(Ip, GlobalIdx);
        uint8_t *GlobalAddr = ModInst->getGlobalAddr(GlobalIdx);
        WASMType GlobalType = ModInst->getGlobalType(GlobalIdx);
        switch (GlobalType) {
        case WASMType::I32:
        case WASMType::F32:
          *(int32_t *)GlobalAddr = Frame->valuePop<int32_t>(ValStackPtr);
          break;
        case WASMType::I64:
        case WASMType::F64:
          *(int64_t *)GlobalAddr = Frame->valuePop<int64_t>(ValStackPtr);
          break;
        default:
          ZEN_ASSERT_TODO();
        }
        BREAK;
      }
      CASE(F32_CONST) : {
        float F32Const;
        Ip = readFixedNumber(Ip, IpEnd, F32Const);
        Frame->valuePush<float>(ValStackPtr, F32Const);
        BREAK;
      }
      CASE(I32_CONST) : {
        int32_t I32Const;
        Ip = readSafeLEBNumber(Ip, I32Const);
        Frame->valuePush<int32_t>(ValStackPtr, I32Const);
        BREAK;
      }
      CASE(F64_CONST) : {
        double F64Const;
        Ip = readFixedNumber(Ip, IpEnd, F64Const);
        Frame->valuePush<double>(ValStackPtr, F64Const);
        BREAK;
      }
      CASE(I64_CONST) : {
        int64_t I64Const;
        Ip = readSafeLEBNumber(Ip, I64Const);
        Frame->valuePush<int64_t>(ValStackPtr, I64Const);
        BREAK;
      }
      CASE(MEMORY_GROW) : {
        Ip = readSafeLEBNumber(Ip, LocalIdx);
        uint32_t GrowOldPageCount = Memory->CurPages;
        uint32_t GrowPageCount = Frame->valuePop<uint32_t>(ValStackPtr);

        if (ModInst->growLinearMemory(0, GrowPageCount)) {
          Frame->valuePush<uint32_t>(ValStackPtr, GrowOldPageCount);
        } else {
          Frame->valuePush<int32_t>(ValStackPtr, -1);
        }
        LinearMemSize = Memory->MemSize;
        BREAK;
      }
      CASE(MEMORY_SIZE) : {
        Ip = readSafeLEBNumber(Ip, LocalIdx);
        Frame->valuePush(ValStackPtr, Memory->CurPages);
        BREAK;
      }
      CASE(F32_STORE) : CASE(I32_STORE) : {
        storeOp<uint32_t, uint32_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                    LinearMemSize);
        BREAK;
      }
      CASE(F64_STORE) : CASE(I64_STORE) : {
        storeOp<uint64_t, uint64_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                    LinearMemSize);
        BREAK;
      }
      CASE(I32_STORE8) : {
        storeOp<uint32_t, uint8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I32_STORE16) : {
        storeOp<uint32_t, uint16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                    LinearMemSize);
        BREAK;
      }
      CASE(I64_STORE8) : {
        storeOp<uint64_t, uint8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I64_STORE16) : {
        storeOp<uint64_t, uint16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                    LinearMemSize);
        BREAK;
      }
      CASE(I64_STORE32) : {
        storeOp<uint64_t, uint32_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                    LinearMemSize);
        BREAK;
      }
      CASE(F32_LOAD) : CASE(I32_LOAD) : {
        loadOp<uint32_t, uint32_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(F64_LOAD) : CASE(I64_LOAD) : {
        loadOp<uint64_t, uint64_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I32_LOAD8_S) : {
        loadOp<uint32_t, int8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                 LinearMemSize);
        BREAK;
      }
      CASE(I32_LOAD8_U) : {
        loadOp<uint32_t, uint8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                  LinearMemSize);
        BREAK;
      }
      CASE(I32_LOAD16_S) : {
        loadOp<uint32_t, int16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                  LinearMemSize);
        BREAK;
      }
      CASE(I32_LOAD16_U) : {
        loadOp<uint32_t, uint16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD8_S) : {
        loadOp<uint64_t, int8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                 LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD8_U) : {
        loadOp<uint64_t, uint8_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                  LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD16_S) : {
        loadOp<uint64_t, int16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                  LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD16_U) : {
        loadOp<uint64_t, uint16_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD32_S) : {
        loadOp<uint64_t, int32_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                  LinearMemSize);
        BREAK;
      }
      CASE(I64_LOAD32_U) : {
        loadOp<uint64_t, uint32_t>(*Memory, Ip, IpEnd, Frame, ValStackPtr,
                                   LinearMemSize);
        BREAK;
      }
      CASE(I64_LT_U) : {
        binaryOp<uint64_t, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_EQZ) : {
        binaryOpNEZ<int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_EQ) : {
        binaryOp<int32_t, BO_EQ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_NE) : {
        binaryOp<int32_t, BO_NE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_NE) : {
        binaryOp<int64_t, BO_NE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_NE) : {
        binaryOp<float, BO_NE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_NE) : {
        binaryOp<double, BO_NE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_LT_S) : {
        binaryOp<int32_t, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_LT_U) : {
        binaryOp<uint32_t, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_GT_S) : {
        binaryOp<int32_t, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_GT_U) : {
        binaryOp<uint32_t, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_LE_S) : {
        binaryOp<int32_t, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_LE_U) : {
        binaryOp<uint32_t, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_GE_S) : {
        binaryOp<int32_t, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_GE_U) : {
        binaryOp<uint32_t, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EQZ) : {
        binaryOpNEZ<int64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EQ) : {
        binaryOp<int64_t, BO_EQ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_LT_S) : {
        binaryOp<int64_t, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_GT_S) : {
        binaryOp<int64_t, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_GT_U) : {
        binaryOp<uint64_t, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_LE_S) : {
        binaryOp<int64_t, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_LE_U) : {
        binaryOp<uint64_t, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_GE_S) : {
        binaryOp<int64_t, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_GE_U) : {
        binaryOp<uint64_t, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_EQ) : {
        binaryOp<float, BO_EQ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_NEG) : {
        binaryOpMath<float, BM_NEG_F32>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_LT) : {
        binaryOp<float, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_GT) : {
        binaryOp<float, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_LE) : {
        binaryOp<float, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_GE) : {
        binaryOp<float, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_EQ) : {
        binaryOp<double, BO_EQ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_NEG) : {
        binaryOpMath<double, BM_NEG_F64>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_LT) : {
        binaryOp<double, BO_LT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_GT) : {
        binaryOp<double, BO_GT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_LE) : {
        binaryOp<double, BO_LE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_GE) : {
        binaryOp<double, BO_GE>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_CLZ) : {
        binaryOpCount<uint32_t, BC_CLZ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_CTZ) : {
        binaryOpCount<uint32_t, BC_CTZ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_POPCNT) : {
        binaryOpCount<uint32_t, BC_POP_COUNT_I32>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_ADD) : {
        binaryOp<int32_t, BO_ADD>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_SUB) : {
        binaryOp<int32_t, BO_SUB>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_MUL) : {
        binaryOp<int32_t, BO_MUL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_DIV_S) : {
        binaryOp<int32_t, BO_DIV_S>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_DIV_U) : {
        binaryOp<uint32_t, BO_DIV>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_REM_S) : {
        binaryOp<int32_t, BO_REM_S>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_REM_U) : {
        binaryOp<uint32_t, BO_REM_U>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_AND) : {
        binaryOp<int32_t, BO_AND>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_OR) : {
        binaryOp<int32_t, BO_OR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_XOR) : {
        binaryOp<int32_t, BO_XOR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_SHL) : {
        binaryOp<int32_t, BO_SHL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_SHR_S) : {
        binaryOp<int32_t, BO_SHR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_SHR_U) : {
        binaryOp<uint32_t, BO_SHR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_ROTL) : {
        binaryOp<uint32_t, BO_ROTL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_ROTR) : {
        binaryOp<uint32_t, BO_ROTR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_CLZ) : {
        binaryOpCount<uint64_t, BC_CLZ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_CTZ) : {
        binaryOpCount<uint64_t, BC_CTZ>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_POPCNT) : {
        binaryOpCount<uint64_t, BC_POP_COUNT_I64>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_ADD) : {
        binaryOp<int64_t, BO_ADD>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_SUB) : {
        binaryOp<int64_t, BO_SUB>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_MUL) : {
        binaryOp<int64_t, BO_MUL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_DIV_S) : {
        binaryOp<int64_t, BO_DIV_S>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_DIV_U) : {
        binaryOp<uint64_t, BO_DIV>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_REM_S) : {
        binaryOp<int64_t, BO_REM_S>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_REM_U) : {
        binaryOp<uint64_t, BO_REM_U>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_AND) : {
        binaryOp<int64_t, BO_AND>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_ABS) : {
        binaryOpMath<float, BM_ABS>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_CEIL) : {
        binaryOpMath<float, BM_CEIL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_FLOOR) : {
        binaryOpMath<float, BM_FLOOR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_TRUNC) : {
        binaryOpMath<float, BM_TRUNC>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_NEAREST) : {
        binaryOpMath<float, BM_NEAREST>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_SQRT) : {
        binaryOpMath<float, BM_SQRT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_ADD) : {
        binaryOp<float, BO_ADD>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_SUB) : {
        binaryOp<float, BO_SUB>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_MUL) : {
        binaryOp<float, BO_MUL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_DIV) : {
        binaryOp<float, BO_DIV>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_MIN) : {
        binaryOp<float, BO_MIN>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_MAX) : {
        binaryOp<float, BO_MAX>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_COPYSIGN) : {
        binaryOp<float, BO_COPYSIGN>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_ABS) : {
        binaryOpMath<double, BM_ABS>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_CEIL) : {
        binaryOpMath<double, BM_CEIL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_FLOOR) : {
        binaryOpMath<double, BM_FLOOR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_TRUNC) : {
        binaryOpMath<double, BM_TRUNC>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_NEAREST) : {
        binaryOpMath<double, BM_NEAREST>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_SQRT) : {
        binaryOpMath<double, BM_SQRT>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_ADD) : {
        binaryOp<double, BO_ADD>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_SUB) : {
        binaryOp<double, BO_SUB>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_MUL) : {
        binaryOp<double, BO_MUL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_DIV) : {
        binaryOp<double, BO_DIV>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_MIN) : {
        binaryOp<double, BO_MIN>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_MAX) : {
        binaryOp<double, BO_MAX>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_COPYSIGN) : {
        binaryOp<double, BO_COPYSIGN>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_WRAP_I64) : {
        Frame->valuePush<int32_t>(
            ValStackPtr, Frame->valuePop<int64_t>(ValStackPtr) & 0xFFFFFFFF);
        BREAK;
      }
      CASE(I32_TRUNC_S_F32) : {
        truncate<int32_t, float, true>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_TRUNC_U_F32) : {
        truncate<int32_t, float, false>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_TRUNC_S_F64) : {
        truncate<int32_t, double, true>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_TRUNC_U_F64) : {
        truncate<int32_t, double, false>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EXTEND_S_I32) : {
        convert<int64_t, int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EXTEND_U_I32) : {
        convert<int64_t, uint32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_TRUNC_S_F32) : {
        truncate<int64_t, float, true>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_TRUNC_U_F32) : {
        truncate<int64_t, float, false>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_TRUNC_S_F64) : {
        truncate<int64_t, double, true>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_TRUNC_U_F64) : {
        truncate<int64_t, double, false>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_CONVERT_S_I32) : {
        convert<float, int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_CONVERT_U_I32) : {
        convert<float, uint32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_CONVERT_S_I64) : {
        convert<float, int64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_CONVERT_U_I64) : {
        convert<float, uint64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F32_DEMOTE_F64) : {
        convert<float, double>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_CONVERT_S_I32) : {
        convert<double, int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_CONVERT_U_I32) : {
        convert<double, uint32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_CONVERT_S_I64) : {
        convert<double, int64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_CONVERT_U_I64) : {
        convert<double, uint64_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(F64_PROMOTE_F32) : {
        convert<double, float>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_REINTERPRET_F32)
          : CASE(I64_REINTERPRET_F64)
          : CASE(F32_REINTERPRET_I32) : CASE(F64_REINTERPRET_I64) : {
        BREAK;
      }
      CASE(I32_EXTEND8_S) : {
        binaryOpCV<int32_t, int8_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EXTEND8_S) : {
        binaryOpCV<int64_t, int8_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I32_EXTEND16_S) : {
        binaryOpCV<int32_t, int16_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EXTEND16_S) : {
        binaryOpCV<int64_t, int16_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_EXTEND32_S) : {
        binaryOpCV<int64_t, int32_t>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_OR) : {
        binaryOp<int64_t, BO_OR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_XOR) : {
        binaryOp<int64_t, BO_XOR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_SHL) : {
        binaryOp<int64_t, BO_SHL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_SHR_S) : {
        binaryOp<int64_t, BO_SHR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_SHR_U) : {
        binaryOp<uint64_t, BO_SHR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_ROTL) : {
        binaryOp<uint64_t, BO_ROTL>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(I64_ROTR) : {
        binaryOp<uint64_t, BO_ROTR>(Frame, ValStackPtr);
        BREAK;
      }
      CASE(RETURN) : {
        Context.freeFrame(FuncInst, Frame);
        InterpFrame *PrevFrame = Frame->PrevFrame;
        ValStackPtr -= (FuncInst->NumReturnCells);
        std::memcpy(LocalPtr, ValStackPtr, FuncInst->NumReturnCells << 2);
        if (PrevFrame == nullptr || !PrevFrame->Ip) {
          return;
        }
        Frame = PrevFrame;
        Context.setCurFrame(Frame);
        // update frame
        updateFrame(Ip, IpEnd, Frame, ValStackPtr, ControlStackPtr, LocalPtr,
                    FuncInst, true);
        BREAK;
      }
      CASE(CALL) : {
        Ip = readSafeLEBNumber(Ip, FuncIdx);
#ifdef ZEN_ENABLE_DEBUG_INTERP
        ZEN_LOG_DEBUG("fidx: %d", FuncIdx);
#endif
        if (FuncIdx == Mod->getGasFuncIdx()) {
          uint64_t Delta = Frame->valuePop<uint64_t>(ValStackPtr);
          uint64_t GasLeft = ModInst->getGas();
          if (GasLeft < Delta) {
            ModInst->setGas(0);
            throw getError(ErrorCode::GasLimitExceeded);
          }
          ModInst->setGas(GasLeft - Delta);

          BREAK;
        }
#ifdef ZEN_ENABLE_CHECKED_ARITHMETIC
        Frame->ValueStackPtr = ValStackPtr;
#define HANDLE_CHECKED_ARITHMETIC_CALL_POSTHOOK                                \
  ValStackPtr = Frame->ValueStackPtr;

        HANDLE_CHECKED_ARITHMETIC_CALL(Mod, FuncIdx)
#undef HANDLE_CHECKED_ARITHMETIC_CALL_POSTHOOK
#endif // ZEN_ENABLE_CHECKED_ARITHMETIC

        FunctionInstance *FuncInstCallee = ModInst->getFunctionInst(FuncIdx);
        callFuncInst(FuncInstCallee, Context, Ip, IpEnd, Frame, ValStackPtr,
                     ControlStackPtr, LocalPtr, FuncInst);
        BREAK;
      }
      CASE(CALL_INDIRECT) : {

        uint32_t TypeIdx = 0, TableIdx = 0;
        Ip = readSafeLEBNumber(Ip, TypeIdx);
        // Skip the fixed byte for `table 0`
        ++Ip;
        auto *ExpectedFuncType = Mod->getDeclaredType(TypeIdx);

        int32_t IndirectFuncIdx = Frame->valuePop<int32_t>(ValStackPtr);
        TableInstance *Table = ModInst->getTableInst(TableIdx);
        if (IndirectFuncIdx < 0 ||
            (uint32_t)IndirectFuncIdx >= Table->CurSize) {
          throw getError(ErrorCode::UndefinedElement);
        }
        FuncIdx = Table->Elements[IndirectFuncIdx];
#ifdef ZEN_ENABLE_DEBUG_INTERP
        ZEN_LOG_DEBUG("fidx: %d", FuncIdx);
#endif
        if (FuncIdx == (uint32_t)-1) {
          throw getError(ErrorCode::UninitializedElement);
        }
        auto *FuncInstCallee = ModInst->getFunctionInst(FuncIdx);
        ZEN_ASSERT(FuncInstCallee);
        auto *ActualFuncType = FuncInstCallee->FuncType;
        if (!TypeEntry::isEqual(ActualFuncType, ExpectedFuncType)) {
          throw getError(ErrorCode::IndirectCallTypeMismatch);
        }
        callFuncInst(FuncInstCallee, Context, Ip, IpEnd, Frame, ValStackPtr,
                     ControlStackPtr, LocalPtr, FuncInst);
        BREAK;
      }
      CASE(END) : {
        if (ControlStackPtr > Frame->CtrlBasePtr + 1) {
          Frame->blockPop(ControlStackPtr);
        } else {
          // return
          Context.freeFrame(FuncInst, Frame);
          InterpFrame *PrevFrame = Frame->PrevFrame;
          ValStackPtr -= (FuncInst->NumReturnCells);
          // copy return value to value stack of prev_frame, frame may
          // be overwrited
          std::memcpy(LocalPtr, ValStackPtr, FuncInst->NumReturnCells << 2);
          Frame = PrevFrame;
          Context.setCurFrame(Frame);

          if (Frame == nullptr) {
            BREAK;
          }

          // update frame
          updateFrame(Ip, IpEnd, Frame, ValStackPtr, ControlStackPtr, LocalPtr,
                      FuncInst, true);
        }
        BREAK;
      }
    DEFAULT : {
      ZEN_LOG_ERROR("munimplemented opcode: 0x%x", Opcode);
      ZEN_ASSERT_TODO();
    }
    }
    // TODO: write back ValueStackPtr, Ip, CtrlStackPtr to Frame
  }
}

void BaseInterpreter::interpret() {
  BaseInterpreterImpl Impl(Context);
  Impl.interpret();
}

} // namespace zen::action

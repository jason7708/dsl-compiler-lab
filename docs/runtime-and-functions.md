# 實體 runtime library 與 DSL 函數呼叫

另有 `--emit-objects` 模式，提供外部 event、context binding、function object 與 DSL 原始碼 library；其型別與呼叫規則見 [function-objects.md](function-objects.md)。下文主要描述既有 scalar 模式。

Compiler 現在從實體 header 讀取數學函數宣告，生成程式呼叫並連結真正的 runtime library。同一份 DSL 輸入也可以定義多個函數，彼此呼叫。

## 一份完整範例

[examples/functions.dsl.cpp](../examples/functions.dsl.cpp)：

```cpp
#include <dsl_runtime/math.h>

double square(double x) {
    return x * x;
}

double length(double a, double b) {
    return dsl_math::sqrt(square(a) + square(b));
}

double compute(double a, double b) {
    return length(a, b);
}
```

`compute` 是對外入口；`square` 和 `length` 是同一個 DSL module 內的 helper。三個函數的 body 都會經過 DSL 規則驗證，並各自建立 typed IR。

```sh
build/dslc examples/functions.dsl.cpp --dump-ir -o build/functions.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/functions.cpp examples/math_driver.cpp \
  build/runtime/libdsl_runtime.a -o build/functions
build/functions
# 5
```

## 真正的 header 與實作

| 檔案／target | 責任 |
| --- | --- |
| [runtime/include/dsl_runtime/math.h](../runtime/include/dsl_runtime/math.h) | 公開 API：`dsl_math::sqrt` 等 double 函數的宣告。 |
| [runtime/src/math.cpp](../runtime/src/math.cpp) | API 的函數定義，目前在這裡呼叫對應的標準數學函式。 |
| [runtime/CMakeLists.txt](../runtime/CMakeLists.txt) | 建立 `dsl_runtime` 靜態 library，並公開 header 路徑。 |
| `build/runtime/libdsl_runtime.a` | 主專案建置時產生的 library，生成程式需要連結它才能解析 runtime 呼叫。 |
| `dsl::runtime` | 給其他 CMake target 使用的 alias。 |

例如 runtime 的實作為：

```cpp
#include "dsl_runtime/math.h"
#include <cmath>

namespace dsl_math {
double sqrt(double x) { return std::sqrt(x); }
}
```

數學演算法目前仍由標準函式庫提供，但呼叫邊界已移到我們自己的 library。Compiler codegen 生成 `dsl_math::sqrt(...)`，不再把這個呼叫改成 `std::sqrt(...)`；日後可在 runtime 實作中替換算法而保留 API。

一般 C++ 程式也可以直接 include 這個 header 並連結 library，不需要經過 `dslc`。

### 單獨建置 runtime

Runtime 自己不依賴 Clang／LLVM，也不使用 compiler 的 expected／format／ranges 等函式庫功能：

```sh
cmake -S runtime -B build-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build-runtime -j2
```

此時 library 路徑是 `build-runtime/libdsl_runtime.a`。在其他 CMake 專案可使用：

```cmake
add_subdirectory(path/to/dsl-compiler-lab/runtime dsl-runtime)
target_link_libraries(your_program PRIVATE dsl::runtime)
```

Include 路徑與 C++23 編譯功能會由 target 傳遞。生成 kernel 的 `-fno-fast-math -ffp-contract=off` 仍需由呼叫端設定。

## 不再注入虛擬標頭

輸入使用正常的 C++ include：

```cpp
#include <dsl_runtime/math.h>
```

`dslc` 在 Clang 的 include 搜尋路徑中加入 configure 時的實體 `runtime/include` 目錄。沒有生成 header 字串、沒有 virtual header mapping，也沒有隱含 `-include`。

因此未 include 時，`dsl_math::sqrt` 會由 Clang 報未宣告。單純四則運算、不使用 runtime API 的 DSL 可以不 include。

原始 token 檢查放行 literal include，include callback 限制為此 runtime header 或 `--extern-header` 登記的實體介面。未登記的 header、macro include、`#define`、`#if`、`#pragma` 等使用者前處理指令仍不支援。引號形式也可使用，但實際解析到的檔案必須是配置的 runtime header。

Include callback 比對 Clang FileEntry 的檔案身分，避免輸入目錄裡另一個同名 `dsl_runtime/math.h` 被當成 runtime API。AST 註冊時也檢查來源檔案與函數簽名，再用 canonical declaration identity 辨識呼叫。

## DSL 函數規則

- 一份輸入是一個 `Module`，可以包含多個全域 DSL 函數。
- 必須有且只有一個名為 `compute` 的入口定義。
- 每個函數的參數及回傳型別都必須是未加限定詞的 double；helper 也遵守相同規則。
- 可以提供前向宣告，但每個宣告都必須在同一輸入檔找到定義。
- 不接受 overload、未登記的外部函數、區域函數宣告或 DSL 輸入內的 namespace 定義。外部 C／C++ library 介面另見 [external-functions.md](external-functions.md)。
- 所有函數 body 都會檢查，包含未被入口使用的 helper。
- 一份 CLI 輸入仍是一個檔案，尚未提供跨檔案 DSL module 連結。

### 前向宣告

遵循一般 C++ 名稱可見性規則：先定義再呼叫，或先寫 prototype。

```cpp
double square(double);

double compute(double x) {
    return square(x);
}

double square(double x) {
    return x * x;
}
```

沒有 prototype 卻呼叫後面才出現的函數，仍由 Clang 報錯。

### 遞迴與相互遞迴

支援直接遞迴與有前向宣告的相互遞迴。例如：

```cpp
double factorial(double n) {
    return n <= 1.0 ? 1.0 : n * factorial(n - 1.0);
}

double compute(double x) {
    return factorial(x);
}
```

條件選值保持 lazy，基底情況不會執行遞迴分支。Compiler 不做遞迴展開或終止性分析。

## AST → Module IR

`HandleTranslationUnit()` 分兩階段處理：

1. 收集實體 runtime header 的 API 與所有使用者函數宣告，檢查簽名、overload、定義與入口，建立 canonical declaration → `FunctionId` 的表。
2. 逐一驗證及 lower 每個函數 body。函數呼叫使用第一階段已建立的表，因此可以指向尚未 lower 的函數或自身。

每個 `Function` 有自己的 values 表與 body region；value ID 在函數內使用，function ID 則在 module 內使用。

`Call` 的目標改成：

```cpp
struct DslFunction { FunctionId id; };
struct ExternalFunction { std::size_t id; };
using CallTarget = std::variant<MathFunction, DslFunction, ExternalFunction>;

struct Call {
    CallTarget target;
    std::vector<ValueId> arguments;
};
```

`MathFunction` 指向已驗證的 runtime API；`DslFunction` 指向 module 中另一個函數。`ExternalFunction` 指向已登記的外部函數。三者都保存 IR 值引用，不複製來源呼叫文字。呼叫只接受直接的 runtime／DSL／已登記外部函數，仍拒絕函數指標與任意 C++ 呼叫。

IR 顯示保留來源函數名稱，例如：

```text
func @square -> double {
  %0 : double = param x
  %1 : double = mul %0, %0
  return %1
}
func @compute -> double {
  %0 : double = param a
  %1 : double = call @square(%0)
  %2 : double = call @dsl_math::sqrt(%1)
  return %2
}
```

## Module codegen

Codegen 先輸出所有函數的 prototype，再輸出每個定義。因此生成結果中的相互呼叫不依賴定義順序。

對外入口保留 `compute` 名稱；helper 以 function ID 產生內部 `static` 名稱，例如 `dsl_function_0`，避免與使用者變數或 runtime API 撞名。

```cpp
#include <dsl_runtime/math.h>

static double dsl_function_0(double v0);
double compute(double v0);

static double dsl_function_0(double v0) {
    const double v1 = (v0 * v0);
    return v1;
}

double compute(double v0) {
    const double v1 = dsl_function_0(v0);
    const double v2 = dsl_math::sqrt(v1);
    return v2;
}
```

`dsl_function_0` 的定義來自 DSL IR；`dsl_math::sqrt` 的定義來自連結的 runtime library，兩者的來源清楚區分。

## 驗證

整合測試新增了 helper 呼叫、前向宣告、入口 prototype、直接／相互遞迴、名稱碰撞、DSL 與 runtime 同名函數、呼叫鏈，以及未定義、overload、無效未使用 helper 等拒絕案例。

Library 邊界也有測試：一般 C++ 直接 include／link 成功；生成的 sqrt 呼叫在不連結 runtime 時會發生 undefined reference。同名替代 header 會被拒絕，既有輸出保持不變。完整執行結果見 [validation.md](validation.md)。

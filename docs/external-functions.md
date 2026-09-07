# 呼叫外部 C／C++ 函數

另有 `--emit-objects` 模式，提供外部 event、context binding、function object 與 DSL 原始碼 library；其型別與呼叫規則見 [function-objects.md](function-objects.md)。下文主要描述既有 scalar 模式。

DSL 可以呼叫實體 header 宣告的外部函數。用可重複的 `--extern-header <path>` 登記允許的介面，DSL 再明確 include 該 header。生成 C++ 保留外部呼叫，由一般 compiler 與 linker 連結你提供的 object 或 library。

## 可執行範例

[examples/external/api.h](../examples/external/api.h) 提供宣告：

```cpp
#pragma once
namespace external_ops {
double weighted_sum(double x, double y);
}
```

[examples/external/api.cpp](../examples/external/api.cpp) 提供真正的 C++23 實作，內部使用 `std::array` 和迴圈，計算 `3*x + 4*y`。它由 host compiler 編譯，不套用 DSL body 規則。

[examples/external.dsl.cpp](../examples/external.dsl.cpp)：

```cpp
#include <api.h>
#include <dsl_runtime/math.h>

double score(double x, double y) {
    return external_ops::weighted_sum(x, y);
}

double compute(double x, double y) {
    return dsl_math::sqrt(score(x, y));
}
```

在 repository 根目錄執行：

```sh
build/dslc examples/external.dsl.cpp \
  --extern-header examples/external/api.h --dump-ir -o build/external.cpp

g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -c examples/external/api.cpp -o build/external_api.o

g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/external.cpp examples/math_driver.cpp \
  build/external_api.o build/runtime/libdsl_runtime.a -o build/external
build/external
# 5（driver 傳入 3 與 4，結果為 sqrt(25)）
```

也可以用自己的 `.a`／`.so` 取代範例 object，並按該 library 的需求提供連結選項。`dslc` 只生成 C++，不負責尋找實作或執行 linker；漏連實作會在 link 階段報 undefined reference。

## 第一版介面範圍

- 支援一般 C++ free function、具名 namespace（可巢狀），以及 `extern "C"` 的 C ABI 函數。
- 回傳值和所有參數都是未加限定詞的 `double`，也接受零參數函數。
- 介面 header 可以使用 include guard、`#pragma once`、`#ifdef __cplusplus`；DSL 本身仍不能使用巨集。
- Header 僅提供函數宣告；不接受函數本體、inline、template、overload、類別、全域變數、指標、reference、variadic、預設參數或 attributes。未使用的宣告也會檢查。
- `compute`、`dsl_function_` 前綴的函數與 namespace 名稱及 runtime 的 `dsl_math` namespace 保留給 compiler／runtime。
- 外部函數不可在 DSL 輸入中再次宣告或定義。DSL helper 的前向宣告仍必須在同一份輸入中有定義。

對於現有的大型 library，提供一份只含上述介面的薄 header，再於 `.cpp` wrapper 中 include 原 library 的完整 header。Wrapper 實作可以使用一般 C++23 功能。

C API 的 header 可寫成：

```cpp
#ifndef NATIVE_API_H
#define NATIVE_API_H
#ifdef __cplusplus
extern "C" {
#endif
double native_scale(double x);
#ifdef __cplusplus
}
#endif
#endif
```

C 實作以 C compiler 編譯成 object；生成的 C++ include 同一份 header，因此保留正確的 C linkage。

## Header 的解析方式

CLI 路徑相對於執行目錄，會解析為實體絕對路徑。每個登記 header 的父目錄加入 Clang include 搜尋路徑，因此通常用 `#include <api.h>` 即可；也接受引號或絕對路徑。若 header 再 include 其他 header，每份依賴也要各自以 `--extern-header` 登記。標準函式庫搜尋仍關閉；標準 header 應放在外部實作檔。

前處理 callback 以實際 FileEntry 身分檢查所有 include。輸入目錄內的同名替代檔不會因此獲得允許。登記不等於隱含 include：DSL 仍要自行引入宣告。

生成 C++ include 本次實際引入的介面 header 的絕對路徑，保留 namespace、C linkage 和宣告，不合成另一套 ABI 宣告。輸出檔可移到其他目錄；搬到另一台機器或搬動介面 header 時需重新生成。介面應在 DSL 編譯與生成程式編譯期間保持一致。

Header 定義的巨集在生成程式 include 完成後會 `#undef`，避免改寫生成的暫存變數或 helper 名稱。這不影響 header 內已完成的條件編譯與宣告。

## AST → IR → C++

1. Frontend 走訪已登記 header 的 namespace／linkage declaration，檢查函數簽名，以 canonical declaration identity 辨識呼叫。
2. 共用 Module 保存 ExternalId 與邏輯 signature，沒有外部函數 body。限定 C++ symbol、真實 header 及巨集資訊另存在 CppLinkage。
3. 呼叫轉成 ExternalCall opcode，arguments 是 typed ValueId；verifier 檢查目標、型別、arity 與 error 契約。
4. C++ backend 由 linkage mapping 生成例如 `::external_ops::weighted_sum(v0, v1)`。Host compiler 從真實 header 取得 ABI，由 linker 找到實作。

`--dump-ir` 以 `extern @ext0` 等 ID 顯示邏輯介面，以 `external_call` 顯示呼叫；來源限定名稱只作為除錯標籤。Object 模式登記的 context provider 則抽到 HostBindings，核心透過普通參數取得準備好的資料。完整分層見 [IR 架構](ir-architecture.md)。

外部函數可以有副作用。DSL 表達式按 IR 順序求值，參數由左到右各求值一次；這明確選定了普通 C++ 函數參數求值順序中原本未指定的順序。`?:` 只執行被選中的 region，外部呼叫不會被提前執行，也不假設它是純函數。外部函數內部的浮點環境、副作用和例外行為由其實作決定。

## 驗證

整合測試分開編譯 C17 object 與 C++23 object，再連結生成程式，驗證 namespace、C linkage、header 依賴、DSL helper → external 呼叫，以及輸出搬移。另驗證漏連實作失敗、外部呼叫次數與參數順序、未選中分支不執行、巨集名稱不改寫生成程式。

拒絕案例涵蓋不支援的簽名與宣告、未登記 header／依賴、同名替代 header、DSL 重新定義外部函數與 CLI 路徑錯誤，並確認失敗不覆蓋既有輸出。實際測試結果見 [validation.md](validation.md)。

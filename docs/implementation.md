# DSL compiler 實作說明

沒有 compiler 背景的讀者，建議先讀 [從零理解這個 DSL compiler](compiler-from-zero.md)：以實際範例逐步說明各層，並對照 Clang／LLVM。

`dslc` 使用 Clang LibTooling 解析受限的 C++23，驗證 DSL 規則後建立獨立 computation IR，再生成 C++23。Compiler、DSL 與生成結果均使用 C++23。目前已有 scalar 函數與 function object 兩種輸出介面，支援實體 math library、DSL helper、外部函數、context binding、struct state 與 typed error。

最新的資料模型、七項 IR 問題的修正與未來 backend 邊界，完整記錄於 [IR 架構](ir-architecture.md)。使用指令見 [README](../README.md)，實測結果見 [validation.md](validation.md)。

## 從來源到生成結果

1. [main.cpp](../src/main.cpp) 解析 CLI／JSON 設定、讀取檔案並檢查輸出路徑。
2. [frontend.cpp](../src/frontend.cpp) 使用 Clang lexer、preprocessor callback 與 AST，檢查所有來源函數、record 及 computation struct。
3. Object 模式經 [frontend_bindings.cpp](../src/frontend_bindings.cpp) 分析呼叫 graph、error contract 與外部 context 需求。
4. [semantic.cpp](../src/semantic.cpp) 把 frontend 私有 source 表示轉成共用 IR，消除來源變數賦值／工作副本，並分開建立 C++ linkage、host bindings、unit envelope metadata。
5. [verify.cpp](../src/verify.cpp) 獨立檢查核心型別、OP、值引用、作用域、呼叫與 terminator。
6. [codegen.cpp](../src/codegen.cpp) 驗證核心與自身支援範圍後生成 C++23。它不使用 Clang AST、frontend 私有表示或來源函數文字。
7. CLI 完整寫入暫存檔後才替換輸出；指定 `--dump-ir` 時，在寫檔成功後輸出核心 IR。

`compile()` 與 `cpp::generate()` 都回傳 `std::expected<結果, std::string>`。Clang 的詳細語法診斷仍包含原始檔名、行、欄。後續 IR／backend 錯誤也會阻止輸出交付。

## 如何擋掉不支援的語法

Clang 負責完整 C++ 的解析與型別分析，DSL 另以允許清單限制語言：

| 檢查層 | 防止漏過的內容 |
| --- | --- |
| Raw token | 未開放的關鍵字、attributes、前處理指令，包括 `#if 0` 隱藏的來源 |
| Include／macro callbacks | 未登記的 header、同名替代 runtime header、DSL 巨集展開 |
| AST declaration／statement／expression | 不支援的型別、轉型、函數簽名、運算及敘述 |
| Object context／contract 分析 | state／mutable local 依賴的 provider、條件讀取、遞迴及不相容 error |
| 共用 IR verifier | 即使不經 Clang 建立 IR，也不能繞過值引用、OP 與控制流程規則 |
| Backend validation | 核心合法但 target 尚未支援的型別、外部 ABI 或不完整 metadata |

例如 scalar 模式的 `a + 1` 因 int → double 隱式轉型被拒絕；object 模式限定允許運算中的 int literal 提升，仍不允許一般 int 變數隱式轉 double。Clang 的普通讀值 cast、限定形式的 C++23 return NoOp 可以移除；其餘轉型須由明確規則接受。

每個 DSL 函數本體都會檢查，包含未被呼叫的 helper。Scalar 與 object 的語法範圍不同，詳見 [數學語法](math.md)、[function objects](function-objects.md) 及 [state／error](state-and-errors.md)。多載 `operator()` 的入口分組、contract 和共用 state 見 [多 event](multi-event.md)。

## 一個計算如何轉換

```cpp
double compute(double a, double b) {
    const double sum = a + b;
    return sum * 0.5;
}
```

Clang 解析優先序並綁定 declaration identity；frontend 移除不改變數值的讀值節點。共用 IR 以參數 ValueId 開始，依序建立 add、identity、constant、mul，最後由 ReturnSuccess 回傳結果。

```text
computation_ir v1
type !0 = ieee754.binary64
type !1 = bool
type !2 = i32
constant #c0 : !0 = bits 0x3fe0000000000000
func @compute ( %0: !0 %1: !0 ) -> ( !0 ) {
  value %0 : !0
  value %1 : !0
  value %2 : !0
  value %3 : !0
  value %4 : !0
  value %5 : !0
  %2 = add %0, %1
  %3 = identity %2
  %4 = constant #c0
  %5 = mul %3, %4
  return_success %5
}
```

Constant 保存精確 bits，不貼回來源字串。Value 只有型別與定義關係；operation 有自己的 ID、operand／result 列表。If 分支以 Yield 交付結果，ReturnSuccess 與 ReturnError 是不同 terminator。

C++ backend 先生成內部函數 prototypes，再依 region 的順序產生運算，最後根據 UnitEnvelope 生成外部 `compute` 或 function object wrapper。內部多結果使用 tuple，typed error 使用 expected；這些 C++ 型別不會出現在核心 IR。

## 函數、state 與 context

數學來源 API 是真實的 `dsl_math::sqrt` 等函數，frontend 把已驗證的 runtime declaration 轉成 Sqrt 等 opcode。C++ backend 再映射到 `dsl_math`，實作連結 [runtime/src/math.cpp](../runtime/src/math.cpp)。ISA backend 可對同一 opcode 選擇自己的指令或數學 library。

Scalar helper 呼叫保存 FunctionId，外部函數保存 ExternalId；限定 C++ 名稱與 include 只在 CppLinkage。Object 模式目前會展開 helper 以收集 context，展開後的計算在共用 IR 中使用 Evaluate 保留獨立 return 邊界。

Struct 成員 state 由 frontend 攤平成 scalar leaves，初值保存到 typed constant pool。Source assignment 轉成新的 value 綁定，field assignment 轉成 Insert，跨分支的 state 透過 Yield 流動。共用函數以普通參數接收 state、以普通成功結果回傳 next state；state 的角色由 UnitEnvelope 指定。

Provider 需求另存在 HostBindings。核心只接收準備好的普通參數，不知道資料來自 get_bid、資料庫或硬體。生成的 `prepare_*_context` 依序呼叫 provider；直接手動提供 context 時不需連結 provider 實作。

DSL `throw Error{...}` 轉成 ReturnError。C++ backend 以 expected 傳播，runtime unit 僅在整個事件成功時提交 new_state。沒有 intent；依賴與條件由計算 graph 表達，外部交付由 host 決定。

## 浮點與 effect

目前沒有最佳化 pass，不做常數折疊、代數重排或自動 FMA。Region 保留運算順序，未選的 If 分支不執行；外部參數的運算按 DSL 定義由左到右求值一次。

生成程式需用 `-fno-fast-math -ffp-contract=off` 編譯。現有執行前提是 IEEE-754 binary64 與一般預設浮點環境，沒有跨平台 libm 最後幾個 bits、extended precision 或動態 rounding mode 的一致性保證。

Registry 對浮點環境與外部呼叫採保守 effect 分類。未來 optimizer 不能把所有帶 result 的 OP 都視為可自由刪除／重排的純函數。

## 模組與建置邊界

| 模組 | 主要檔案 | 依賴 |
| --- | --- | --- |
| `dsl_ir` | [ir.h](../include/dsl/ir.h)、[registry.cpp](../src/registry.cpp)、[verify.cpp](../src/verify.cpp)、[ir.cpp](../src/ir.cpp) | C++23 標準函式庫 |
| `dsl_cpp_backend` | [program.h](../include/dsl/program.h)、[codegen.cpp](../src/codegen.cpp) | dsl_ir，沒有 LLVM／Clang |
| `dsl_frontend` | frontend、frontend_bindings、semantic | Clang／LLVM 20.1.8、dsl_ir；供 CLI 與來源位置 API 測試共用 |
| `dslc` | config、main | dsl_frontend、dsl_cpp_backend、LLVM |
| `dsl_runtime` | [math.h](../runtime/include/dsl_runtime/math.h)、[math.cpp](../runtime/src/math.cpp)、[operation.h](../runtime/include/dsl_runtime/operation.h) | 標準 C++23；供生成程式使用 |

`DSL_BUILD_COMPILER=OFF` 可只建 core／backend 與直接 API 測試。Frontend 私有 [frontend_ir.h](../src/frontend_ir.h) 仍保存來源導向的過渡結構，但不在公開 IR API，也不由 backend 使用。這項區分與原因見 [架構文件](ir-architecture.md)。

## C++23 寫法與驗證

使用 `std::expected` 傳遞錯誤，variant 表達互斥型別／terminator，strong ID 避免混用引用，span／string_view 提供非擁有 view，format／print 產生診斷，RAII 管理暫存輸出。Clang AST 的非擁有指標與 dyn_cast 保留其既有 ownership 模型。

測試分為直接建構 IR 的 verifier tests、直接呼叫 C++ backend 的 metadata／capability tests、透過公開 compile API 核對來源位置再刻意破壞 IR 的診斷測試，以及真實 CLI → 生成 → 編譯 → 執行的兩組 Python suites。除了預期結果，也比對原始 C++ 的 double bits、state 生命週期、錯誤提交邊界、provider 次數、拒絕診斷及既有輸出保護。實測數字與命令見 [validation.md](validation.md)。

目前尚未實作 ISA、serializer 或 optimizer；這次提供的是它們可共用且能獨立驗證的 IR 邊界。

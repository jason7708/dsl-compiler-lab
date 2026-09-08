# 用 C++ struct 表達計算 state

Object 模式使用普通 C++ struct 成員表示 state，計算寫在 `operator()`。沒有持久 state 的計算仍可寫普通函數。Compiler、DSL 與生成結果均為 C++23；不需要特殊 attribute、虛擬 header 或自訂 parser。

## 宣告與初始值

```cpp
struct Event { double value; bool enabled; };
struct Result { double total; bool positive; };
struct Error { int code; };

struct Accumulator {
    double total = 0.0;

    Result operator()(Event event) {
        if (!event.enabled || event.value < 0.0) {
            throw Error{.code = 1};
        }
        total = total + event.value;
        if (total > 100.0) {
            throw Error{.code = 2};
        }
        return Result{.total = total, .positive = total > 0.0};
    }
};
```

Compiler 把 `total` 提取成 `Accumulator_state` 欄位，初始值保存於生成的 state 型別。預設建立 `unit<Accumulator>` 時採用這些初值，不會每次事件重新初始化。Host 也可提供初始 state。

```cpp
dsl_runtime::unit<Accumulator> first; // total = 0.0
dsl_runtime::unit<Accumulator> second(Accumulator_state{10.0});
auto response = first.on_event({}, Event{5.0, true});
// response->result.total == 5.0
// first.state().total == 5.0；second.state().total == 10.0
```

生成的 `Accumulator` object 沒有隱藏 state；公開執行介面仍為 `operator()(const context&, const state&, const event&)`，回傳 `std::expected<output, error>`。State 由 unit 持有，output 僅包含 `result` 與 `new_state`。

## 同一實例共用，不同實例獨立

```cpp
struct Twice {
    Accumulator accumulator;

    Result operator()(Event event) {
        Result first = accumulator(event);
        if (first.positive) return accumulator(event);
        return first;
    }
};
```

兩次呼叫同一個 `accumulator`，會共用累計值。初始 total 為 0、event.value 為 10 時，第一次得到 10，第二次得到 20。下一個成功事件繼續使用該實例保存的 total。

如果需要兩份狀態，就宣告兩個普通成員：

```cpp
struct Pair {
    Accumulator bid;
    Accumulator ask;

    Result operator()(Event event) {
        Result first = bid(event);
        return ask(event);
    }
};
```

`bid` 與 `ask` 獨立。生成的 state 會攤平為 `bid_total`、`ask_total`，沒有子物件 pointer 或動態配置。更深的子物件依成員路徑繼續攤平；若攤平欄位名稱撞名，compiler 會診斷，不會讓兩份狀態意外合併。不同最外層 unit 各自擁有整份 state。

函數內的一般區域物件遵循 C++ 生命週期：

```cpp
Result once(Event event) {
    Accumulator temporary;
    return temporary(event);
}
```

`temporary` 每次呼叫重新建立，不會持久保存。`once_state` 是空型別。這避免把區域物件暗中改成 static。計算可以使用具名 member 或 local instance，暫不接受 `Accumulator{}(event)` 這種臨時接收物件的呼叫。

## Error 與 state 提交

DSL 的 `throw Error{...}` 是 typed error 語法，降成共用 IR 的 `ReturnError` terminator，再生成 `std::unexpected`。生成的 operation 沒有 C++ throw／exception unwinding，已以 `-fno-exceptions` 編譯執行測試。DSL 不支援 try/catch 或 rethrow。

每次 operation 在 state 副本上工作。子實例成功時，將其 new_state 合併到 caller 對應的工作欄位；下一次呼叫同一實例會看到這些更新。子計算出錯會立即向外傳播，停止後續計算。

最外層 `unit.on_event` 成功才提交整份 new_state。例如前一個子實例成功、後一個子實例失敗，unit 的所有 state 欄位都維持事件開始前的值。直接呼叫 object 只回傳 new_state，不修改傳入的 const state。

同一條呼叫鏈目前共用一種 flat error struct，caller 會繼承 callee 的 error 型別；不相容的 error 型別會被拒絕。沒有 error 路徑就生成空 error。NaN／Inf 不會自動轉成 error。

同一個 unit 的事件由 host 依序呼叫，沒有內建並行排程。State commit 要求不拋例外的複製指派與 response 移動建構。

## Computation graph 與輸出

Intent 已從 runtime、contract 與 output 移除。DSL 的函數／實例呼叫描述靜態依賴，`if/else`、`?:`、短路邏輯決定執行時走哪條分支。沒有額外的路由欄位或 send 指令。

Host 從成功的 `response->result` 取得結果，決定如何對接框架或外部輸出函數。目前 graph 是 compiler 分析及展開的呼叫關係，沒有獨立的 graph scheduler。任意動態目的地與外部發送副作用不在這個 contract 裡。

## Context 的邊界

外部讀取仍在 `prepare_*_context(event)` 執行。Provider 不可依賴計算成員 state、被修改的 local 或中間計算，也不能放在條件分支或可能提前 return／error 的路徑之後；經由子物件呼叫的讀取也會檢查。

例如先讀取、再驗證：

```cpp
double bid = get_bid(event.instrument_id);
double ask = get_ask(event.instrument_id);
if (!event.enabled || ask < bid) throw Error{1};
```

即使 event 最後被拒絕，binder 仍會讀 bid／ask。Provider 的讀取副作用不會因 state 未提交而撤銷；一致性及讀取失敗由 host 處理。Provider 的 C++ 例外不會自動變成 DSL error。

## 第一版 struct 支援範圍

| 項目 | 規則 |
| --- | --- |
| 計算 struct | 全域 public aggregate struct，在 struct 內定義 `operator()`，可加 const；多載入口須各有一個不同型別的 event，詳見 [多 event](multi-event.md)。 |
| Scalar 成員 | double、bool、32-bit int，每個都必須有明確 literal 初值，可用正負數及 `{}` 零初始化。初值不接受函數呼叫或計算式。 |
| 子計算成員 | 另一個計算 struct，可用預設初始化或 literal aggregate 初始化覆蓋初值。 |
| Local 實例 | 普通 default／aggregate 初始化，每次執行重新建立；不支援複製實例或整個實例賦值。 |
| 成員存取 | 讀取與 `=`，包含 `total`、`this->total`、`child.total` 及更深子成員。 |
| Event／result／error | primitive 或 flat record；計算物件不能作為 event、result 或 error 傳遞。 |
| 分支與運算 | 保留 if/else、early return、bool 短路、double 數學與 flat record result。未選分支不執行子計算。 |

拒絕 constructor、destructor、繼承、virtual、其他 method、operator overload、static／mutable 成員、reference／pointer、陣列。普通資料 record 作為計算成員尚未支援，可先展開 scalar 欄位。資料 struct 的規則仍維持 flat、無 method、無欄位初值。

`auto`、迴圈、`+=`、`++`、一般數值 cast 和整數算術仍未支援。Int 可以儲存、傳遞與比較；數值計算以 double 為主。State 結構展開深度上限 64，object call graph 上限 256；object IR 展開上限 100000 值。

攤平後的 contract 資料固定大小，並有 standard-layout／trivially-copyable 檢查。C++ struct 仍可能有 padding；`std::expected` 不代表已定義的硬體 ABI。位元布局、硬體 backend 及排程尚未實作。

## 舊寫法遷移

先前的 `Result f(Event event, State& state)` 已不再接受。將 State 欄位搬進 `struct f`，把本體改為 `Result operator()(Event event)`，移除 `state.`；要持久呼叫 f 的父計算則宣告 f 成員。

Runtime 的 contract 現在有五個型別參數：`contract<Context, State, Event, Result, Error>`。`operation_output<Result, State>` 只含 result／new_state。舊生成 header 必須重新生成；手寫 C++ operation 也要移除 Intent 型別參數及 output 的 intent initializer。

## 完整 library 範例

[examples/stateful](../examples/stateful) 包含外部 market header、`read_quote` 普通 DSL 函數、`accumulate_value` 計算 struct，以及組合它們的 `accumulate_mid`。

```sh
build/dslc examples/stateful/accumulate.dsl.cpp \
  --config examples/stateful/project.json --dump-ir \
  -o build/accumulate.generated.h
g++ -std=c++23 -Wall -Wextra -Werror -O2 \
  -fno-fast-math -ffp-contract=off -Iruntime/include -Ibuild \
  examples/stateful/driver.cpp build/runtime/libdsl_runtime.a \
  -o build/accumulate
build/accumulate
```

Driver 驗證成功提交與錯誤保留 state，輸出：

```text
total = 204, average = 102
error = 2, retained total = 204
invalid quote = 1, provider reads = 8
```

## 共用 IR 中的 state／error

來源 struct 與其初值由 frontend 分析，成員更新轉成不可變 record 的 Extract／Insert；跨分支更新使用 Yield。核心函數以普通參數接收 state，以 ReturnSuccess 的普通結果回傳下一個 state，state 角色另在 UnitEnvelope 指定。

ReturnError 與 ReturnSuccess 分離；核心沒有 C++ throw、expected 或工作副本。C++ backend 選擇用區域副本實作 Insert，runtime unit 才決定何時提交。Frontend 私有 source planning 仍保留來源導向的過渡操作，但它們不會交給 backend。完整說明見 [IR 架構](ir-architecture.md)。

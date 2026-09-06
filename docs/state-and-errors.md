# 條件、結構化 result、state 與 error

這些功能使用 `--emit-objects`，compiler 與生成結果均為 C++23。既有 scalar 模式的限制不變；新計算建議使用 object 模式。Intent 仍為空型別，沒有路由或發送行為。

## DSL 作者如何寫

```cpp
struct Event { double value; bool enabled; };
struct State { double total; };
struct Result { double total; bool positive; };
struct Error { int code; };

Result accumulate(Event event, State& state) {
    if (!event.enabled || event.value < 0.0) {
        throw Error{.code = 1};
    }
    state.total = state.total + event.value;
    if (state.total > 100.0) {
        throw Error{.code = 2};
    }
    return Result{.total = state.total, .positive = state.total > 0.0};
}
```

最後一個 `State&` 參數明確表示持久 state；沒有這個參數就生成空 state。State 不放在 function object 的隱藏欄位裡，由 caller 傳入，再透過成功 output 的 `new_state` 回傳。

`throw Error{...}` 是 DSL 的 typed error 語法。Compiler 將它降成 error return，生成 `std::unexpected(error)`，不生成 C++ throw，也不用 exception unwinding。生成的 operation 已以 `-fno-exceptions` 編譯並執行測試。不可使用 try/catch、rethrow 或任意例外型別。

Error 必須是平坦 struct；同一函數及其呼叫鏈共用一種 error struct。沒有自身 throw 的 caller，會繼承 callee 的 error 型別。若組合了不同 error 型別，目前會拒絕；library 作者可共用 `CalcError { int code; }`，以 code 區分錯誤。沒有錯誤路徑就生成空 error。

## 分支與區域資料

| 功能 | 支援方式 |
| --- | --- |
| 條件 | `if / else`、巢狀 block、early return；每條路徑都必須 return 或 typed throw。 |
| 邏輯 | bool 的 `!`、`&&`、`||`；後兩者短路求值。數值不可直接當條件。 |
| 選值 | `condition ? a : b`，兩邊同型別，允許 primitive 或 flat record。 |
| 區域變數 | 顯式型別且必須初始化，可加 const；支援 scalar 的 `=` 及 local record 欄位的 `=`。 |
| 結構化資料 | flat record 的聚合初始化、指定欄位初始化、複製、欄位讀取與回傳；省略欄位補零。 |
| 結果型別 | `double`、`bool`、32-bit `int` 或 flat record。 |
| State 更新 | `state.field = expression;`，可在分支及 helper 裡使用。 |

Event 參數只讀。整個 record 的重新賦值、`+=`、`++`、`auto`、迴圈、陣列、pointer、一般 cast、整數算術目前仍不支援。Int 可儲存、傳遞及比較；數值計算仍以 double 為主，例如 count 可以先用 double 欄位。

## 組合與提交語意

新作者仍然用普通 function call 組合 library：

```cpp
Result twice(Event event, State& state) {
    Result first = accumulate(event, state);
    if (first.positive) return accumulate(event, state);
    return first;
}
```

每次 helper 成功後，其 `new_state` 成為 caller 的工作 state；後續 helper 會看到更新值。任何 helper 出錯，error 自動向外傳播，後續計算停止。短路或未選中的分支不會執行 helper，也不會產生它的 error 或 state 更新。

最外層 `unit.on_event` 成功才提交 state；即使第一個 helper 成功、第二個 helper 失敗，unit 仍保留整次事件開始前的 state。直接呼叫 object 則只回傳 `new_state`，不修改傳入的 const state。

```cpp
dsl_runtime::unit<twice> calculation(State{0.0});
auto response = calculation.on_event({}, Event{10.0, true});
if (response) {
    // response->result.total == 20.0
    // calculation.state().total == 20.0
} else {
    // response.error().code；unit 的舊 state 保留。
}
```

不同 unit 各自持有 state。預設建構會將 state value-initialize，也可由 host 指定初始值。同一 unit 的事件由外部依序呼叫。

目前 stateful helper 必須收到 caller 的那一個明確 state 參數，而且型別相同。不支援把任意 local 當作子 operation 的持久 state，也沒有替每個 call site 自動配置獨立子 state。Event／其他值參數會複製；即使 event 值來自 state，helper 的 state 更新也不會改變已複製的 event。

## 與 context 的邊界

外部讀取仍在 `prepare_*_context(event)` 執行。Provider 不能依賴 state、被修改的 local 或中間計算，也不能放在條件分支或可能提前 return／error 的路徑之後。否則 binder 會提早執行原本可能不該發生的讀取，因此 compiler 會拒絕。

需要讀資料再驗證時，DSL 明確先讀：

```cpp
double bid = get_bid(event.instrument_id);
double ask = get_ask(event.instrument_id);
if (!event.enabled || ask < bid) throw CalcError{1};
```

這表示即使 event 最後被拒絕，binder 仍會讀 bid／ask。Provider 的讀取副作用無法靠 state rollback 撤銷；snapshot、一致性及讀取失敗由 host 實作處理。Provider 的 C++ 例外不會自動變成 DSL typed error，NaN／Inf 也不會自動轉成 error。

## Library 與完整執行範例

[examples/stateful](../examples/stateful) 包含外部 market header、可重用的 `read_quote`／`accumulate_value` DSL library、新作者的 `accumulate_mid`，以及 host driver。它同時示範 provider、分支、record result、state、typed error 和 JSON dependency config。

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

預期輸出（CTest 也編譯執行這個 driver）：

```text
total = 204, average = 102
error = 2, retained total = 204
invalid quote = 1, provider reads = 8
```

## 平坦記憶體的範圍

Event、state、result、error 都只接受 public、trivial、standard-layout 的 flat struct，欄位為 double／bool／32-bit int；禁止巢狀 record、繼承、method、pointer、array、欄位預設初值。Context 展開為 scalar 欄位。生成碼對所有 contract 資料型別及 output 加入 standard-layout／trivially-copyable 檢查。

這些是 host 的固定大小值型別，沒有動態容器。C++ struct 仍可能含 padding，`std::expected` 的布局也不是硬體 ABI。位元布局、數值格式、硬體 backend 與排程仍未實作。

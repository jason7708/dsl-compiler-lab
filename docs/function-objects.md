# Function object、context binding 與計算 library

`--emit-objects` 將 DSL 函數生成為 C++23 function object，並提供 `context / state / event / result / error / intent` contract、context 準備函數，以及管理 state 的 `unit`。Intent 暫時是空型別，沒有 send、emit 或路由語意。

## 完整範例

外部介面 [examples/operations/market.h](../examples/operations/market.h)：

```cpp
#pragma once
struct ext_event {
    int instrument_id;
    double reference_price;
};
double get_bid(int id);
double get_ask(int id);
```

Library 作者撰寫 [pricing.dsl.h](../examples/operations/pricing.dsl.h)：

```cpp
#pragma once
#include "market.h"

double mid_price(ext_event event) {
    double bid = get_bid(event.instrument_id);
    double ask = get_ask(event.instrument_id);
    double mid = (bid + ask) / 2;
    return mid;
}
```

新的 DSL 作者可以直接 include 並組合它：

```cpp
#include "pricing.dsl.h"

double price_difference(ext_event event) {
    double mid = mid_price(event);
    return mid - event.reference_price;
}
```

編譯命令：

```sh
build/dslc examples/operations/price_difference.dsl.cpp \
  --emit-objects \
  --extern-header examples/operations/market.h \
  --dsl-library examples/operations/pricing.dsl.h \
  --context-function get_bid --context-function get_ask \
  --dump-ir -o build/prices.generated.h

g++ -std=c++23 -Wall -Wextra -Werror -O2 \
  -fno-fast-math -ffp-contract=off \
  -Iruntime/include -Ibuild \
  examples/operations/driver.cpp examples/operations/market.cpp \
  build/runtime/libdsl_runtime.a -o build/price_objects
build/price_objects
```

實際輸出：

```text
difference = 2
manual mid = 12
```

## 各種檔案與 CLI 的責任

| 選項／檔案 | 意義 |
| --- | --- |
| `--config path` | 載入可組合的 JSON 依賴設定；見 [library-config.md](library-config.md)。 |
| `--emit-objects` | 生成可 include 的 C++23 header；每個 DSL 函數都有 object，不要求 `compute` 入口。 |
| `--extern-header path` | 登記外部事件型別與函數宣告的實體 header。 |
| `--context-function qualified_name` | 將某個外部 double 函數明確登記為 context provider；可重複指定。 |
| `--dsl-library path` | 登記包含 DSL 函數宣告／定義的實體原始碼 library；可重複指定。 |
| DSL 的 `#include` | 讓 Clang 取得宣告；登記檔案不會隱含 include。 |
| 生成的 `.h` | 提供給一般 C++ 呼叫端使用的 objects、contracts 和 binding 函數。 |

所有登記檔案以實體檔案身分比對；每個檔案父目錄加入 include 搜尋路徑。外部介面與 DSL library 不可登記成同一個檔案。輸入、已登記的 header／library 也不可被 output 覆蓋。

`--emit-objects` 之外的既有 scalar 模式保持原來的語法與直接外部呼叫行為。

## 生成的介面

以 `mid_price` 為例，公開介面相當於：

```cpp
struct mid_price_context { double bid; double ask; };
using mid_price_event = ext_event;
struct mid_price_state {};
using mid_price_result = double;
struct mid_price_error {};
struct mid_price_intent {};

using mid_price_contract = dsl_runtime::contract<
    mid_price_context, mid_price_state, mid_price_event,
    mid_price_result, mid_price_error, mid_price_intent>;
using mid_price_output = mid_price_contract::output;

struct mid_price {
    using contract_t = mid_price_contract;
    contract_t::response operator()(
        const mid_price_context& ctx,
        const mid_price_state& state,
        const mid_price_event& event) const;
};

mid_price_context prepare_mid_price_context(const mid_price_event& event);
```

`contract_t::output` 的實體結構是 `operation_output<Result, Intent, State>`，包含 `result`、`intent`、`new_state`。生成的 `mid_price_output` 與 contract 使用同一型別，不是兩個形狀相同但互不相容的 struct。`contract_t::response` 為 `std::expected<output, error>`。

Object 沒有隱藏的可變 state。其 `operator()` 從 context 讀取 bid、ask，從 typed IR 生成計算，最後回傳 output。生成的 binder 則依序呼叫真正的 `get_bid/get_ask`。

Host 可以自行準備 context：

```cpp
const ext_event event{.instrument_id = 0, .reference_price = 99.0};
const mid_price_context ctx{.bid = 10.0, .ask = 14.0};
auto result = mid_price{}(ctx, {}, event);
// result->result == 12.0；沒有呼叫 get_bid/get_ask。
```

若呼叫端完全不使用 binder，就不需要連結 provider 的實作。Header 中仍需有宣告。所有生成的 binder 都是 inline，object 的函數定義位於 class 內，允許同一份生成 header 用於多個 C++ translation units。

單一 record 參數會成為 event alias。純 scalar 函數的所有參數則組成 event struct；零參數函數得到空 event。例如 `double square(double x)` 生成 `square_event { double x; }`。未命名參數會取得不與原有欄位衝突的生成名稱。

## 讀取與計算分成兩階段

`--context-function` 明確選擇以下語意：

1. 外部提供 event。
2. `prepare_*_context(event)` 依展開後的 DSL 呼叫順序執行 context provider。
3. `operator()(ctx, state, event)` 執行其餘計算，不再呼叫 provider。

這是 object 模式的階段界線；不等同於任意原始 C++ 外部呼叫與數學運算交錯執行。讀取的一致性、來源與可能的副作用由 provider 實作負責，compiler 不承諾快照或自動將兩次讀取合併。Host 應讓 binding 與 evaluation 使用對應的 event。

目前 provider 回傳 double，參數可以是 double、bool 或 32-bit int。參數來源限 event 欄位、原函數 scalar 參數、literal 常數或這些值未被修改的區域別名；不可依賴 state。尚不接受由中間計算或另一個 provider 的結果決定下一次讀取。

條件分支內、可能提前 return／error 之後的 provider 讀取也會拒絕，包含經由 helper 間接讀取。這避免把未選中的讀取無條件提早執行。例如：

```cpp
return event.enabled ? get_bid(event.instrument_id) : 0.0; // 拒絕
```

可以明確選擇先取得資料，再條件計算：

```cpp
double bid = get_bid(event.instrument_id);
return event.enabled ? bid : 0.0;
```

一般數學函數與純 DSL helper 仍可放在 `?:` 裡，生成程式保留分支的延遲執行。

## Library 如何組合

DSL library 是 compiler 會讀取及驗證的原始碼 library，可以使用 `#pragma once` 與 literal include；不接受 `#define`／條件編譯或巨集展開。外部介面 header 則仍可使用一般 include guard。

Frontend 先建立整個 module 的函數表。Object lowering 在 typed IR 層展開 DSL 呼叫，複製 callee 的值參數，為 state 建立工作副本；每個外部讀取轉成明確的 `ContextRead`。因此 `price_difference` 的 context 會包含它呼叫的 `mid_price` 所需資料，例如：

```cpp
struct price_difference_context {
    double mid_price_0_bid;
    double mid_price_0_ask;
};
```

重複呼叫同一個 helper 會保留各次 provider 讀取，並生成不同欄位。這保留讀取次數與順序，不假設外部資料在兩次呼叫間不會變動。直接綁定到區域變數的讀取優先使用該變數名稱；其他讀取使用生成名稱。這些名稱由 module 結構決定，不承諾跨版本穩定 ABI。

Object 模式拒絕遞迴／相互遞迴，單一 object 展開上限為 100000 個 IR 值；scalar 模式原有遞迴功能不受影響。所有 library 函數本體都會檢查，即使未被呼叫。

目前每次輸出是一份包含本 module 所有 objects 的 header bundle。這不是已編譯 DSL binary 的匯入機制；新 DSL 重用的是 `.dsl.h` 原始碼與外部介面登記資訊。生成 header 給一般 C++ 使用，不應再當成 DSL 原始碼 include。多份 bundle 若包含同名 object，應合併來源生成一次，避免重複定義。

## State、error 與 unit

DSL 可用最後一個 `State& state` 參數宣告持久 state，以 `state.field = value` 更新；`throw Error{...}` 表達 typed error，生成 `std::unexpected`，不產生 C++ throw。回傳值可以是 double、bool、int 或 flat record。沒有宣告時，state／error 仍各自生成空型別。

[operation.h](../runtime/include/dsl_runtime/operation.h) 的 `unit<Op>` 管理提交：

- 每個 unit 各自擁有一份 state，可指定初始值。
- `on_event` 接收 const context 與 const event，呼叫 operation。
- expected 成功才提交 `new_state`；失敗保留舊 state 並原樣回傳 error。
- 要求 state 複製指派與 response 移動建構不拋例外，避免提交後的回傳因移動失敗。
- 同一個 unit 的事件由外部依序調用；沒有內建並行排程或輸出佇列。

DSL helper 的 state／error 會依呼叫鏈組合：成功更新 caller 的工作 state，失敗自動向外傳遞；最外層 unit 成功才提交。現階段整條鏈共用同一種 error record，stateful helper 必須接收 caller 明確的同型別 State&。

Context binder 直接回傳 context；provider 的 C++ 例外由 host 處理，不自動包成 DSL error。NaN／Inf 也不自動變成 error。具體語法、rollback 測試與完整 library 範例見 [state-and-errors.md](state-and-errors.md)。

## 支援型別與記憶體

Event、state、result 與 error record 必須是平坦、trivial、standard-layout 的 public struct，欄位只接受 double、bool、32-bit int。拒絕指標、陣列、巢狀 record、繼承、member function、bit-field、欄位初值與其他 modifiers。Frontend 檢查 Clang 目標中的 int 寬度為 32 bits。

Object 模式支援已初始化的 scalar／record 區域變數、scalar 賦值、local record 欄位更新、if/else、early return 與 bool 短路運算。回傳型別可以是 double、bool、int 或 flat record。允許 double 運算中的 int literal 提升，例如 `/ 2`；一般 int 變數不會隱式轉成 double。另支援 int 對 int 的比較，適合比較 instrument ID；尚未加入整數算術。

生成的 context／state／event／output 不使用動態容器或 owning pointer，context 依賴展開成 scalar 欄位。生成碼會檢查 context、event、state、result、error 與 output 的 standard-layout／trivially-copyable 性質。

這些是目前的 host C++ 型別，仍可能有 padding；`std::expected` 也不是已定義好的硬體 ABI。硬體封裝、位元布局、數值格式與 scheduling 尚未實作，不宣稱目前的生成 header 可以直接綜合成硬體。

## 實作位置

| 檔案 | 責任 |
| --- | --- |
| [frontend.cpp](../src/frontend.cpp) | Object 模式的 record、欄位讀取、型別檢查、DSL library 載入與 provider 登記。 |
| [ir.h](../include/dsl/ir.h) | Record metadata、int／record 型別、Member、ContextRead 與函數資訊。 |
| [objects.cpp](../src/objects.cpp) | 展開 DSL 呼叫、驗證 context 依賴、產生 binder、contract 與 object。 |
| [operation.h](../runtime/include/dsl_runtime/operation.h) | 共用 contract／output 與 unit。 |
| [objects.py](../tests/objects.py) | 生成碼編譯執行、context 邊界、library 組合及 unit state 驗證。 |

Object codegen 只接收 typed IR，沒有複製 DSL 原始碼作為函數本體。

# 多種 event，共用一個計算物件

一個 struct 可以定義多個 `operator()`，用不同 event 型別選擇入口。寫 DSL 的人仍使用普通 C++ 的 `tracker(event)`；同一個物件的入口共用成員，不同物件各自擁有 state。

```cpp
struct QuoteEvent { double bid; double ask; };
struct TradeEvent { double price; bool valid; };
struct TradeError { int code; };

struct PriceTracker {
    double mid = 0.0;
    double last_trade = 0.0;

    double operator()(QuoteEvent event) {
        mid = (event.bid + event.ask) / 2.0;
        return mid;
    }

    bool operator()(TradeEvent event) {
        last_trade = event.price;
        if (!event.valid) throw TradeError{1};
        return last_trade > mid;
    }
};
```

每次 event 到達都會執行對應入口。這裡不是「等兩種 event 都到齊」的配對機制；如果需要等待，必須明確設計儲存資料、就緒狀態與是否輸出的規則。

## Host 使用方式

```cpp
dsl_runtime::unit<PriceTracker> tracker;
const QuoteEvent quote{100.0, 104.0};
auto q = tracker.on_event(prepare_PriceTracker_context(quote), quote);
auto t = tracker.on_event({}, TradeEvent{103.0, true});
auto failed = tracker.on_event({}, TradeEvent{999.0, false});
```

`q` 的 result 是 double，`t` 的 result 是 bool。第三次呼叫回傳 TradeError，state 保留第二次成功後的值：mid 為 102，last_trade 為 103。直接呼叫生成的 `PriceTracker{}(ctx, state, event)` 只回傳新 state，不修改傳入的 state。

`prepare_PriceTracker_context(event)` 依 event 型別多載，只準備該入口需要的 context。沒有 provider 的入口可直接傳入 `{}` 作為 context。Event 需有明確型別，例如 `TradeEvent{...}`；只有 `{...}` 無法讓多入口 unit 推導 event 型別。

若需要取得型別：

```cpp
using QuoteContract = PriceTracker::contract_for<QuoteEvent>;
using TradeContract = PriceTracker::contract_for<TradeEvent>;
// QuoteContract::state 和 TradeContract::state 都是 PriceTracker_state。
// context、event、result、error、output、response 則依入口決定。
```

每個入口可以有不同 result 與 error；同一個入口組合 helper 時，仍遵守既有「共用一種 error record」規則。呼叫者須依序處理同一個 unit 的事件，不支援同時寫入同一個 unit。

## DSL 內的組合

可以把 PriceTracker 放進另一個計算 struct，或建立 local PriceTracker，再用不同 event 呼叫。Clang 的 C++ 多載解析決定要呼叫哪個入口。成員物件的入口共用持久 state；local 物件的 state 每次外層計算重新初始化。外層事件失敗時，子物件的變更也不提交。

## IR 與 C++ 的分工

每個入口都是獨立的核心 IR function，接受 event、舊 state、context，產生 result、新 state 或 error。核心 OP 不新增 C++ method、overload 或 dispatch 語意。

`UnitEnvelope::groups` 透過 FunctionId 記錄哪些入口屬於同一個 unit。C++ backend 檢查入口均已 export、event 型別各不相同，而且 state 型別及初值相同，才生成多載物件及 `contract_set`。這些分組資料不放入 computation IR，其他 backend 可以依自己的執行介面處理。

生成 header 另外保留 `PriceTracker_entry_0`、`PriceTracker_entry_1` 的入口包裝及對應型別，編號依來源宣告順序。一般 host 程式應使用 `PriceTracker`、`contract_for<Event>` 和 `prepare_PriceTracker_context(event)`，避免依賴編號。`unit<PriceTracker>` 持有一份 state；分別建立兩個 entry 的 unit 會各自持有 state。

資料仍為固定大小的值，state 仍展平成 scalar 欄位，不需要 heap、函數指標或虛擬函數。這不代表已定義跨 backend 的二進位 ABI；硬體 layout 仍需由未來的 target 決定。

## 目前限制

- 多載入口每個都必須有一個 event 參數，event 型別必須不同；支援既有的 double、bool、int 及 flat record。
- 不能只靠 const／非 const 區分同型別 event 的入口。
- 單入口 struct 保留原有參數及 host API，不受上述多載參數數量限制。
- 具名 member function、一般全域函數多載、event 自動配對尚未支援。

## 可執行範例

來源：[multi_event.dsl.cpp](../examples/multi_event.dsl.cpp)，driver：[multi_event_driver.cpp](../examples/multi_event_driver.cpp)。

```sh
build/dslc examples/multi_event.dsl.cpp --emit-objects -o build/multi_event.h
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off -fno-exceptions \
  -Iruntime/include -Ibuild examples/multi_event_driver.cpp \
  build/runtime/libdsl_runtime.a -o build/multi_event
build/multi_event
```

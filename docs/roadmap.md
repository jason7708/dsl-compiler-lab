# 架構問題與後續工作

記錄日期：2026-09-08。這份文件保存目前的缺陷、限制與建議順序，避免把「已建立架構邊界」誤認為「所有 backend／library 情境都已成熟」。

本輪授權範圍是記錄所有項目，並處理第 1 項。其餘項目是後續待辦。

| 順序 | 項目 | 狀態 |
| --- | --- | --- |
| 1 | 不同生成 library 的內部名稱與連結隔離 | 已完成並通過回歸驗證 |
| 2 | 明確的 OP 數值／effect 契約與 IR 參考解譯器 | 待處理 |
| 3 | 保留 object Call，分開 context 分析與函數展開 | 待處理 |
| 4 | Library export 選擇與 error 型別轉換 | 待處理 |
| 5 | 選定 ISA，建立具體 target backend | 待處理 |

## 1. 不同生成 library 的名稱與連結隔離

### 已確認的錯誤

原本每份輸出都使用相同的 `dsl_backend` namespace，函數再以 module 內部的 FunctionId 命名為 `f0`、`f1` 等。FunctionId 只在各自 module 內有意義，因此不同 library 的 f0 可能有完全不同的函數本體。

已用以下兩個來源分別生成 object header 重現：

```cpp
// 第一份 DSL
double plus_one(double x) { return x + 1.0; }
```

```cpp
// 第二份 DSL
double times_two(double x) { return x * 2.0; }
```

| 使用方式 | 修正前的實際結果 |
| --- | --- |
| 同一份 C++ 檔案 include 兩份 header | `dsl_backend::f0(double)` 重複定義，編譯失敗。 |
| 兩份 C++ 檔案各 include 一份 header，編譯後連結 | 本機 GCC 在 `-O0` 下，兩個函數輸入 10，得到 11、11；正確結果應為 11、20。 |

這是會造成錯誤結果的缺陷。原本「同一份生成 header 被多個 C++ 檔案使用」的測試不能涵蓋「不同生成 header 一起使用」。

### 本次修正

C++ backend 從 `UnitEnvelope.exports` 選出字典序最小的公開名稱，作為生成 bundle 的命名依據：

```cpp
namespace dsl_backend::module_plus_one {
    // f0 等內部函數
}
namespace dsl_backend::module_times_two {
    // 另一份 bundle 的 f0 等內部函數
}
```

公開 wrapper 使用完整限定名稱呼叫所屬 bundle 的內部函數。Scalar 與 object 的 wrapper 都使用這個規則；bundle 內部的 helper Call 仍在自己的 namespace 中解析。

可共存的 bundle 必須沒有衝突的公開 export。兩組互不重疊的 export 集合，其最小名稱必然不同，因此這個命名方式不需要雜湊、隨機 ID 或來源路徑；也沒有新增 LLVM／Clang 依賴。

C++ backend 現在明確要求至少一個 export，避免產生沒有公開用途、也沒有命名依據的 bundle。共用 IR 仍允許沒有 export 的計算集合，這項限制屬於 C++ 輸出介面。

穩定性的範圍是：相同 export 集合不受 export 排列、來源／輸出檔名或生成程序影響。若新增更小的 export 名稱，內部 namespace 會改變；內部名稱不是承諾跨版本穩定的公開 ABI。

### 驗收項目

- 不同 object header 在同一份 C++ 檔案共存，交換 include 順序也正確。
- 各自編譯為 `.o` 再連結，交換連結順序也得到 11、20。
- 上述情況均驗證 `-O0` 與 `-O2`。
- Scalar 與 object bundle 一起連結，內部同編號 helper 不互相干擾。
- 同一份 header 跨多個 C++ 檔案使用的既有行為保留。
- 來源與輸出搬移／改名不改變無外部路徑依賴範例的生成內容。
- 直接 backend API 驗證 export 順序不影響 namespace，空 export 有明確診斷。
- 完整 CLI 整合測試與不載入 LLVM／Clang 的獨立建置通過。

本次實測：原始跨檔錯誤案例已得到正確的 11、20；4 個 CTest 全部通過（55.65 秒），獨立 core／backend 的 2 個 CTest 也通過。測試總數為 49 個 verifier 案例、16 個 backend 邊界案例與 40 個 Python 測試方法。

相關實作：[codegen.cpp](../src/codegen.cpp)。測試：[objects.py](../tests/objects.py)、[backend.cpp](../tests/backend.cpp)。實測紀錄：[validation.md](validation.md)。

### 保留的限制

這項修正隔離的是內部實作名稱，不會替使用者改寫公開名稱。兩份 bundle 若都公開同名 object、wrapper 或互相衝突的 record，仍應整合來源一次生成，或使用不同公開名稱。同一公開介面的不同版本也不能因此任意混合連結。

既有生成檔需要重新生成；這次沒有改 DSL 語法或新增 CLI 參數。

## 2. OP 語意契約與 IR 參考解譯器

### 問題

Registry 已集中 opcode、型別／arity 規則與 effect 分類，但數學行為仍部分依賴文件、目前的 C++ runtime 與 host 浮點環境。TypeId 寫出 binary64，並不足以說明不同 backend 對所有邊界值應如何一致。

需要明確決定捨入、NaN、負零、除零、數學函數誤差範圍，以及 errno／浮點旗標是否屬於 DSL 的可觀察契約。目前沒有一般 effect 推導 pass。

Verifier 檢查結構合法性，不檢查 frontend／backend 是否保留計算意思；例如減法錯轉成加法，結構仍可能合法。

### 建議實作與驗收

先記錄各 OP 可測試的語意與 target 支援邊界，再建立直接執行共用 IR 的參考解譯器。它不模擬 CPU，而是處理值、Call、If、Evaluate、成功／失敗與 state 結果。

同一份 IR 分別由解譯器及 C++ backend 執行，對照 result、error、new_state、外部呼叫次數與順序。解譯器若共用 host 數學 library，仍需另外加入獨立的數值邊界／誤差測試，不能把相同 library 的一致輸出當成跨平台數學證明。

## 3. 保留 Call，分開 context 分析與函數展開

### 問題

Object frontend 現在為了收集 context，會展開每個 helper，再以 Evaluate 保留 return 邊界。重複使用大型 helper 時，IR／生成碼會重複膨脹，原本的函數呼叫結構也難以保留。現有 100000-value 展開限制只能阻止無限制增長，不能解決規模問題。

### 建議實作與驗收

每個函數保留自己的 IR，另外計算 context 需求摘要。呼叫時傳入對應 context／state 值，是否 inline 則成為獨立、可選的轉換階段。

不同呼叫的 provider 結果必須維持各自的次數與順序，不能把分析摘要的重用誤做成 runtime 讀取合併。分支、early return、error 傳播與整次事件的 state 提交語意也必須保留。

驗收應包含重複 helper 與多層 library 組合的程式大小／建置成本，以及相同來源在轉換前後的行為對照。

## 4. Export 選擇與 error 組合

### 問題

Object 模式現在把所有 DSL 函數都公開，缺少「公開 operation」與「內部 helper」的選擇。Scalar 模式則仍強制 compute 作為入口；共用 IR 本身沒有這個限制。

此外，不同 library 的 error record 不同時，現在不能直接組成同一條 error 傳播鏈。

### 建議實作與驗收

提供清楚的 export 選擇，優先考慮設定或普通函數介面，避免讓作者學習大量特殊語法。讓組合者能明確把不同 library error 轉成自己的 error 型別，並決定未處理錯誤如何被拒絕。

驗收包括公開介面可控、內部 helper 不再無條件輸出，以及不同 error library 的顯式轉換、傳播與 state 提交行為。

## 5. 選定 ISA 與硬體資料布局

目前有共用 IR 邊界，不等於任何 ISA 已可直接執行。平坦 record 也不等於固定的硬體 bytes；仍要定義 padding、alignment、endianness、數值格式與呼叫慣例。

應先選定具體 target 與最小計算集合，定義 context／event／state 的輸入輸出、error 表示、指令選擇與外部操作映射，再用第 2 項的對照基準驗收。

ISA backend、硬體 ABI、一般 IR 序列化與最佳化 pipeline 都尚未實作。本輪不將它們列為已完成。

## 追蹤修正：空分支引用與 verifier 來源位置

本輪另確認並修正兩個問題，未改變上方尚未完成的架構待辦。

### 空分支也必須重映射結果

Object frontend 的 cloneBranch 曾以「分支是否有 instructions」決定是否 remap result。但 `c ? v : 0.0` 的 true 分支只是引用既有 v，可以沒有指令，卻仍必須交付正確的值。舊程式會把結果留在預設 ID 0；型別不同時被 verifier 錯誤拒絕，型別相同時可能算錯。

現在依分支是否需要產生值決定：Select 的兩側總是重映射 result，包含 `?:`、`&&`、`||`；If 的 statement 分支不讀取無意義的 result。

新增測試將 bool 真值組合、參數／local 引用、巢狀 `?:`、helper 呼叫中改變的 ValueId 與空 statement 分支，對照同一份來源的普通 C++ 執行結果。使用者提供的 f／g 兩例也納入其中。

### Verifier 使用真實 OP／terminator 位置

原本 verifier 雖然會讀 op.location，semantic 卻把所有 OP 都設成函數起點；遞迴檢查也可能讓子 region 的位置留在診斷狀態。

現在來源位置從 Clang 的 expression／statement 傳到私有來源表示，再經 clone／semantic 傳入核心。OP 保存運算子位置，region 與 terminator 分別保存區域／return／yield 位置；這些都是與 backend 無關的診斷 metadata。

Verifier 進入與離開函數、OP、region 時保存／還原位置，OP 錯誤另外附 OperationId。沒有來源位置的手動 IR 不會捏造行列，仍提供可用的函數／OP 身分。

新增的 `frontend_locations` 測試使用公開 compile API 建立真實 IR，再刻意破壞 operand 或 yield，檢查原始檔行／欄、helper 原始位置以及父計算位置的還原。Frontend 的四份實作抽成 dsl_frontend library，讓測試與 CLI 使用同一份程式，不新增測試用的 CLI 注入開關。Core／C++ backend 仍可獨立建置。


## 已完成：多 event 的 struct 入口

支援以不同 event 型別多載 `operator()`。每個多載須有一個 event 參數，具名 member function 和自動配對 event 留待後續討論。入口各自有 context／result／error，共用同一份 struct state，成功才提交；成員及 local 計算物件都可用普通 C++ 呼叫語法組合。

核心 IR 維持普通函數，新增分組僅位於 UnitEnvelope.groups。C++ backend 驗證分組後生成多載 function object、按 event 選擇的 contract_set 和 context binder；unit 仍持有固定大小的 state。單入口 API 保持相容。範例與限制見 [multi-event.md](multi-event.md)。

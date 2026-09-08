# 本次建置與驗證紀錄

日期：2026-09-08。以下為完成共用 IR 分層、獨立 verifier、C++ backend 遷移，修正 object 空分支結果重映射與 verifier 來源位置，並加入多 event struct 入口後，在此 workspace 實際重新建置與執行的結果。

## 環境

| 項目 | 實際版本／位置 |
| --- | --- |
| 平台 | Linux x86_64 |
| CMake | 3.29.2 |
| C++ 標準（compiler、DSL 輸入、生成結果） | C++23 |
| LLVM | 20.1.8 |
| Clang LibTooling（DSL frontend） | Ubuntu Clang 20.1.8 |
| Host C++ compiler／標準函式庫 | GCC 16.1.0／libstdc++ 16.1.0 |
| LLVM config | `/usr/lib/llvm-20/lib/cmake/llvm` |
| Clang shared library | `/usr/lib/llvm-20/lib/libclang-cpp.so.20.1` |
| Python | 3.10.12 |
| Runtime header | `runtime/include/dsl_runtime/math.h` |
| Runtime static library | `build/runtime/libdsl_runtime.a` |

`build/dslc --version` 實際輸出：

```text
dslc 0.1.0
LLVM 20.1.8
Clang Ubuntu clang version 20.1.8 (++20250708082409+6fb913d3e2ec-1~exp1~20250708202428.132)
```

初始環境已有 LLVM 開發套件與 Clang shared library，但缺少 `clang/Tooling/Tooling.h`。下載匹配的 `libclang-20-dev` Debian 套件並解壓縮到 `/tmp`，沒有修改系統安裝：

```sh
mkdir -p /tmp/dsl-llvm20
cd /tmp/dsl-llvm20
apt-get download libclang-20-dev
dpkg-deb -x libclang-20-dev_*.deb /tmp/dsl-llvm20/root
```

實際套件版本：

```text
1:20.1.8~++20250708082409+6fb913d3e2ec-1~exp1~20250708202428.132
```

若 `/tmp` 被清理，需要重新解壓縮標頭，或依 README 安裝完整開發套件並重新 configure。此環境專用的路徑只存在建置設定與這份紀錄，不寫死在 compiler 原始碼中。

## Host 標準函式庫

原本 `/usr/bin/clang++` 自動選擇 `/usr/include/c++/11`，實際編譯功能探測程式時出現 `fatal error: 'expected' file not found`。本機的 `/usr/bin/g++` 是 GCC 16.1.0，使用 `/opt/gcc-16.1` 的標頭與 runtime，已成功編譯、連結並執行使用 expected／format／print／enumerate 的探測程式。

因此本次改用 GCC 16.1.0 作為 host compiler，Clang LibTooling 依賴仍固定為 20.1.8。CMake 新增標準函式庫功能檢查，避免只有 `-std=c++23` 旗標卻缺少對應函式庫的情形。

## 實際建置命令

在 repo 根目錄：

```sh
cmake --fresh -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER=gcc \
  -DDSL_CLANG_INCLUDE_DIR=/tmp/dsl-llvm20/root/usr/lib/llvm-20/include
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CMake configure 成功，`dsl_ir`、`dsl_cpp_backend`、`dsl_frontend`、`dslc` 與三個 C++ 測試程式編譯並連結成功；runtime 的 `math.cpp` 另編譯成 `libdsl_runtime.a`。LLVM configure 顯示未找到 LibEdit、zstd、CURL 等選用依賴；此工具使用的 AST／Tooling 路徑不需要它們，沒有阻擋建置或測試。

## 實際測試結果

本次五個測試項目最終均通過，完整套件與後續針對受影響項目的重跑結果列於下表。測試過程修正了測試程式的 IR 欄位順序與既有限制不相容的案例；一輪整合測試曾與 dslc 重新連結重疊而失敗，已在建置完成後分別重跑 scalar／object 套件通過。

| CTest 項目 | 結果 | 時間 |
| --- | --- | --- |
| ir_verifier | 通過 | 0.00 秒 |
| backend_boundary | 通過 | 0.00 秒 |
| frontend_locations | 通過 | 0.02 秒 |
| compiler_integration | 通過 | 52.80 秒 |
| object_integration | 通過 | 7.82 秒 |

這是 **5 個 CTest entries**：54 個直接構造 IR 的 verifier 案例、23 個 backend API 邊界案例、11 個 frontend 來源位置案例，以及 **43 個 Python unittest 方法**（scalar／external 15 個、object 28 個）。Core-only 建置的兩個 CTest 項目也全部通過。

Verifier 案例涵蓋型別／值／OP／constant ID、arity、定義唯一性、ancestor capture／sibling escape、call signature、error、missing／invalid terminator、欄位及 registry。Backend 案例直接建構 Program，驗證 source-independent codegen、metadata 拒絕、backend 自行驗證核心，以及「共用 IR 接受 binary32、目前 C++ backend 明確拒絕」的能力分工。

本次空分支回歸測試包含使用者提供的 f／g、bool 真值組合、local 引用、巢狀條件、helper 展開後的 ValueId 變更與空 statement 分支，將生成結果對照相同來源的普通 C++。來源位置測試透過公開 compile API 取得 IR，再破壞 operand／yield，確認 verifier 指向實際的運算子、helper 原始位置或 return／yield，而非函數起點。另直接構造 IR，確認遞迴驗證後會還原父 OP 的診斷位置。

多 event 測試涵蓋不同 record／scalar event 分派、每個入口的 context／result／error contract、交錯事件共用 state、不同 unit 隔離、初始化覆寫、typed error 回滾、直接呼叫不改動輸入，以及 member／local 多載呼叫組合。Backend 另驗證不經 frontend 的多入口 Program，拒絕缺少或重複的入口、重複 event 型別、名稱碰撞與不一致的 state。`examples/multi_event.dsl.cpp` 與 driver 已依 [multi-event.md](multi-event.md) 命令生成、以 C++23／`-fno-exceptions` 編譯並執行，退出碼為 0。

其中資料驅動案例包含：

- Object suite：context 準備／純計算分工、無 provider 實作的手動 context、多次呼叫不合併讀取、DSL library 組合、延遲數學分支、生成 header 多個 translation units，以及外部 record／library／CLI 拒絕案例。
- 真正 DSL 的 stateful helper 組合、成功提交、第二個 helper 失敗時整次事件 rollback、不同 unit 隔離，以及直接呼叫不改動輸入 state。
- if/else、early return、local scalar／record 欄位賦值、作用域遮蔽、record result 及 bool／int 回傳。
- 短路和未選取分支不執行 fallible helper；typed error 自動傳播。生成程式另以 `-fno-exceptions` 編譯執行。
- 同一子物件重複呼叫共用 state、不同子物件隔離、三層組合與初值，對照相同原始 struct 的普通 C++ 執行結果。
- 區域計算物件每次事件重新建立；literal 初值、子物件 aggregate 初值覆蓋、this／巢狀 scalar 成員讀寫。
- 不同子物件的失敗提交邊界、const operator、provider 抽取，以及生成碼不含 intent。
- 18 組 struct 拒絕案例：舊 reference state、未初始化／非 literal 初值、constructor／其他 method、state 欄位撞名、計算物件 event、條件及 state 依賴 provider。
- 14 組新增 feature 拒絕案例，涵蓋不同 error type、state 來源、提前退出後的 context 讀取及尚未支援的語法。
- JSON 相對路徑、重複 imports、CLI 組合、imported config 覆寫保護，以及 9 組無效設定案例和缺少 CLI 參數。
- Repo 的 stateful library 範例作為 object suite 的一個端到端測試。
- 58 組成功生成、編譯並執行的運算案例，在 `-std=c++23 -O2 -fno-fast-math -ffp-contract=off` 下同時對照原始 C++ 與已知預期值，採 double 位元比較。
- 98 組基本語法拒絕案例，檢查非零退出碼、原始碼位置、原因、無 IR 輸出，以及不改動既有輸出。
- 9 組多函數案例，包含 helper、前向宣告、入口 prototype、直接／相互遞迴、名稱碰撞與呼叫鏈。
- 外部 C17 與 C++23 object 分開編譯後連結，驗證 C linkage、巢狀 namespace、header 依賴、helper 呼叫與生成檔搬移。
- 外部函數的呼叫次數、參數由左到右求值、lazy 條件分支與 header 巨集隔離。
- 22 組外部介面表列拒絕案例，加上外部重新宣告／定義、未登記 header、同名替代檔與 CLI 檢查。
- Runtime 的普通 C++ include／link、漏連結時的 undefined reference，以及同名 header 替換拒絕檢查。
- 原始驗收範例與新增數學範例的 IR、編譯及執行檢查。
- 四組條件分支的 FE_INVALID／FE_DIVBYZERO／errno 檢查，確認未選中的 sqrt、log、除法及巢狀分支沒有執行。
- sqrt／log／pow 的 NaN／Inf、fmin／fmax 的 NaN，以及 NaN 相等／不相等比較的執行檢查。
- 零參數、未命名參數的生成程式編譯檢查。
- 精確錯誤行／欄檢查。
- CLI、輸入不存在、同檔案／symlink 保護、輸出目錄不存在、輸出寫入失敗的檢查。

先前切換 C++23 時，既有測試發現直接回傳參數／區域變數時，Clang 會加入 `NoOp` lvalue-to-xvalue 隱式節點。Lowering 已限定接受不改變 double 型別和值的此類引用，並增加兩組括號回傳與負零的測試。目前使用實體 runtime header／library，核心以 registered math opcode、Call 與 ExternalCall 表示運算；外部介面由可重複的 `--extern-header` 登記。改用實體 header 後，3 組參數數量錯誤測試的預期診斷改為 Clang 實際的 too many／too few arguments；上面結果是完成修改後的完整重跑。

另已檢查 `build/compile_commands.json`，確認各模組均使用 `-std=c++23`；`build/build-info.txt` 也記錄 host 與 DSL／生成結果的 C++23 設定。

## 驗收範例的端到端執行

```sh
build/dslc examples/average.dsl.cpp --dump-ir -o build/average.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/average.cpp examples/driver.cpp \
  build/runtime/libdsl_runtime.a -o build/average
build/average
```

實際 stdout 為 `3`，退出碼為 0。核心 IR 已列於 README；目前 workspace 可直接檢查 `build/average.cpp` 並執行 `build/average`。

本次以 GCC 16.1.0／libstdc++ 16.1.0 建置 compiler 和生成程式，DSL 解析連結 Clang LibTooling 20.1.8，於 Linux x86_64 驗證；未宣稱已在其他作業系統、架構或 LLVM 版本通過。

## 新增數學範例

```sh
build/dslc examples/math.dsl.cpp --dump-ir -o build/math.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/math.cpp examples/math_driver.cpp \
  build/runtime/libdsl_runtime.a -o build/math
build/math
```

實際 stdout 為 `5`、退出碼為 0；driver 同時檢查 `(0.0, 0.0)` 得到 `0.0`。完整使用方式與分支 IR 說明見 [math.md](math.md)。

## 多函數範例與獨立 library 建置

```sh
build/dslc examples/functions.dsl.cpp --dump-ir -o build/functions.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/functions.cpp examples/math_driver.cpp \
  build/runtime/libdsl_runtime.a -o build/functions
build/functions
```

實際 stdout 為 `5`、退出碼為 0。IR 包含 square、length、compute 三個函數，以及 DSL helper Call 和數學 opcode。

也已實際執行：

```sh
cmake -S runtime -B build-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build-runtime -j2
```

Configure 與建置成功，產生 `build-runtime/libdsl_runtime.a`，過程不需要 LLVM／Clang 開發依賴。此獨立 library 只使用自身 header 與標準數學函式庫。


## 外部函數範例

```sh
build/dslc examples/external.dsl.cpp \
  --extern-header examples/external/api.h --dump-ir -o build/external.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -c examples/external/api.cpp -o build/external_api.o
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/external.cpp examples/math_driver.cpp \
  build/external_api.o build/runtime/libdsl_runtime.a -o build/external
build/external
```

實際 stdout 為 `5`、退出碼為 0。IR 包含以 ExternalId 表示的邏輯外部介面，以及 `score`、`compute` 的 body；生成 C++ 呼叫 `::external_ops::weighted_sum`，實作來自另編譯的 object。介面限制與 C ABI 範例見 [external-functions.md](external-functions.md)。


## Function object 與 context binding

```sh
build/dslc examples/operations/price_difference.dsl.cpp \
  --emit-objects --extern-header examples/operations/market.h \
  --dsl-library examples/operations/pricing.dsl.h \
  --context-function get_bid --context-function get_ask \
  --dump-ir -o build/prices.generated.h
g++ -std=c++23 -Wall -Wextra -Werror -O2 \
  -fno-fast-math -ffp-contract=off -Iruntime/include -Ibuild \
  examples/operations/driver.cpp examples/operations/market.cpp \
  build/runtime/libdsl_runtime.a -o build/price_objects
build/price_objects
```

實際 stdout：

```text
difference = 2
manual mid = 12
```

退出碼為 0。外部 provider 只出現在生成的 `prepare_*_context`；`operator()` 從 context 讀取資料。新測試也驗證手動 context 不需連結 provider，以及同一份生成 header 可以跨 C++ translation units 使用。此無 state 範例生成空 state／error；下面的範例則使用 DSL struct 宣告實際 state。Intent 已從 runtime 與生成介面移除。

本次 `/tmp` 中的 Clang 開發標頭已被清除，重新下載同版本 `libclang-20-dev` 並解壓到原路徑後成功建置，沒有修改系統安裝。一次 object 測試曾發現未命名參數的生成欄位與使用者的 `arg_0` 撞名，已修正命名並在完整回歸中通過。完整介面與限制見 [function-objects.md](function-objects.md)。


## Stateful library 完整範例

```sh
build/dslc examples/stateful/accumulate.dsl.cpp \
  --config examples/stateful/project.json --dump-ir \
  -o build/accumulate.generated.h > build/accumulate.ir.txt
g++ -std=c++23 -Wall -Wextra -Werror -O2 \
  -fno-fast-math -ffp-contract=off -Iruntime/include -Ibuild \
  examples/stateful/driver.cpp build/runtime/libdsl_runtime.a \
  -o build/accumulate
build/accumulate
```

本次實際 stdout：

```text
total = 204, average = 102
error = 2, retained total = 204
invalid quote = 1, provider reads = 8
```

退出碼為 0。Driver 驗證成功事件提交，超過上限的 error 保留 total／count，錯誤報價自動向外傳播，以及 binder 的讀取次數。完整語意見 [state-and-errors.md](state-and-errors.md)。

本次已將範例改為 `struct accumulate_mid` 持有 `accumulate_value accumulator` 成員，生成 `accumulator_total`／`accumulator_count` 兩個平坦 state 欄位。預設 unit 使用成員初值，provider 與 typed error 行為保留。

本次 build 所用 `/tmp` Clang 標頭已被清除，重新下載並解壓相同版本的 libclang-20-dev 後完成建置；未變更系統安裝。完整回歸結果如上，包含既有 scalar／external suite 與新增 struct 實例測試。

## 獨立 core／backend 建置

```sh
cmake -S . -B build-core -G Ninja \
  -DDSL_BUILD_COMPILER=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build build-core -j2
ctest --test-dir build-core --output-on-failure
```

Configure 沒有查找 LLVM／Clang，產生 `libdsl_ir.a` 與 `libdsl_cpp_backend.a`。兩個 CTest 項目 `ir_verifier`、`backend_boundary` 全部通過。此模式不建立 CLI 或 runtime，證明公開核心與 C++ backend 能在沒有 frontend 連結依賴的情況下使用。

IR dump 的既有文字斷言已更新為 `computation_ir v1`；端到端運算、provider、state／error、浮點 bits 與拒絕行為維持測試。尚未實作或驗證 ISA target、跨平台硬體 ABI、serialization 或 optimizer，詳見 [IR 架構](ir-architecture.md)。

## 不同生成 library 的名稱隔離修正

修正前用 plus_one／times_two 兩份不同 bundle 重現內部 f0 衝突：同檔 include 編譯失敗，分檔在 GCC `-O0` 連結執行得到 11、11，預期為 11、20。

修正後 backend 以最小 export 名稱建立各自 namespace，原始重現案例已得到 11、20。Object suite 新增三個測試方法，涵蓋兩份 header 在 `-O0`／`-O2` 下交換 include 順序、真正分別編譯成 `.o` 並交換連結順序、scalar／object 混合連結，以及來源／輸出搬移的穩定生成。

Backend API 另驗證獨立 namespace、export 排序穩定性及空 export 的拒絕。上方 CTest 數字是完成此次修正後的完整重跑；獨立 `DSL_BUILD_COMPILER=OFF` 建置也重新通過。完整問題與保留限制見 [工作紀錄](roadmap.md)。

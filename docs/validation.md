# 本次建置與驗證紀錄

日期：2026-09-06。以下為加入 object 控制流程、record result、state／typed error 與 library JSON 設定後，在此 workspace 實際重新建置與執行的結果。

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

CMake configure 成功，compiler 的 5 個 C++ translation units 編譯並連結成功；runtime 的 `math.cpp` 另編譯成 `libdsl_runtime.a`。LLVM configure 顯示未找到 LibEdit、zstd、CURL 等選用依賴；此工具使用的 AST／Tooling 路徑不需要它們，沒有阻擋建置或測試。

## 實際測試結果

```text
1/2 Test #1: compiler_integration ............. Passed 45.85 sec
2/2 Test #2: object_integration ............... Passed  4.24 sec
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 50.09 sec
```

這是 **2 個 CTest entries、32 個 Python unittest 方法**：scalar／external suite 15 個，object suite 17 個。其中資料驅動案例包含：

- Object suite：context 準備／純計算分工、無 provider 實作的手動 context、多次呼叫不合併讀取、DSL library 組合、延遲數學分支、生成 header 多個 translation units，以及外部 record／library／CLI 拒絕案例。
- 真正 DSL 的 stateful helper 組合、成功提交、第二個 helper 失敗時整次事件 rollback、不同 unit 隔離，以及直接呼叫不改動輸入 state。
- if/else、early return、local scalar／record 欄位賦值、作用域遮蔽、record result 及 bool／int 回傳。
- 短路和未選取分支不執行 fallible helper；typed error 自動傳播。生成程式另以 `-fno-exceptions` 編譯執行。
- 值參數與 state 同源時仍保留 event 副本的語意。
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

先前切換 C++23 時，既有測試發現直接回傳參數／區域變數時，Clang 會加入 `NoOp` lvalue-to-xvalue 隱式節點。Lowering 已限定接受不改變 double 型別和值的此類引用，並增加兩組括號回傳與負零的測試。目前使用實體 runtime header／library，IR 有 Module、DSL function 與 external function call target；外部介面由可重複的 `--extern-header` 登記。改用實體 header 後，3 組參數數量錯誤測試的預期診斷改為 Clang 實際的 too many／too few arguments；上面結果是完成修改後的完整重跑。

另已檢查 `build/compile_commands.json`，確認 compiler 的 6 個 translation units 均使用 `-std=c++23`；`build/build-info.txt` 也記錄 host 與 DSL／生成結果的 C++23 設定。

## 驗收範例的端到端執行

```sh
build/dslc examples/average.dsl.cpp --dump-ir -o build/average.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/average.cpp examples/driver.cpp \
  build/runtime/libdsl_runtime.a -o build/average
build/average
```

實際 stdout 為 `3`，退出碼為 0。IR 與生成的 C++ 已列於 README；目前 workspace 可直接檢查 `build/average.cpp` 並執行 `build/average`。

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

實際 stdout 為 `5`、退出碼為 0。IR 包含 square、length、compute 三個函數，以及 DSL helper 和 runtime API 兩種 call。

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

實際 stdout 為 `5`、退出碼為 0。IR 包含 `extern @external_ops::weighted_sum(double, double) -> double`，以及 `score`、`compute` 的 body；生成 C++ 呼叫 `::external_ops::weighted_sum`，實作來自另編譯的 object。介面限制與 C ABI 範例見 [external-functions.md](external-functions.md)。


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

退出碼為 0。外部 provider 只出現在生成的 `prepare_*_context`；`operator()` 從 context 讀取資料。新測試也驗證手動 context 不需連結 provider，以及同一份生成 header 可以跨 C++ translation units 使用。此無 state 範例生成空 state／error／intent；下面的新增範例則使用 DSL 宣告實際 state 與 error。Intent 路由仍未加入。

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

第一輪回歸捕捉到擴充 NoOp 判斷時誤擋 scalar const double 回傳，已修正並重跑全部測試。原本拒絕 local 賦值與 record local 的 object 案例改成實際運算驗證；不完整 return 的預期診斷採 Clang `-Werror` 實際輸出。上述 50.09 秒是完成這些修正後的完整通過結果。

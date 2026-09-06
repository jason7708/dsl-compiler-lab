# 可重用的 library 設定

Library 作者可以把來源、外部介面與 provider 登記一起放進 JSON，使用者只需 include DSL library，並把設定交給 compiler。

[範例 library.json](../examples/stateful/library/library.json)：

```json
{
  "emit_objects": true,
  "external_headers": ["market.h"],
  "dsl_libraries": ["pricing.dsl.h"],
  "context_functions": ["get_bid", "get_ask"]
}
```

使用者的 [project.json](../examples/stateful/project.json)：

```json
{
  "imports": ["library/library.json"]
}
```

```sh
build/dslc examples/stateful/accumulate.dsl.cpp \
  --config examples/stateful/project.json -o build/accumulate.generated.h
```

| 欄位 | 型別與用途 |
| --- | --- |
| `imports` | string array：先載入其他 config，支援遞迴組合。 |
| `emit_objects` | bool：啟用 function object 輸出。 |
| `external_headers` | string array：等同 `--extern-header`。 |
| `dsl_libraries` | string array：等同 `--dsl-library`。 |
| `context_functions` | string array：等同 `--context-function`，使用完整限定名稱。 |

所有欄位可省略；未知欄位、錯誤型別、空字串、無效 JSON、不存在的檔案與循環 import 都會診斷。Import 深度上限 64 層，同一份設定的重複 import 只載入一次。

所有檔案路徑以**該 JSON 所在目錄**為基準，與執行 compiler 的工作目錄無關。路徑會解析成既有實體檔案。依賴 header／library 的父目錄會加入 include 搜尋路徑；DSL 仍必須明確 `#include`，登記不會自動插入來源內容。

`--config` 可重複指定，並與既有 CLI flags 累加。`emit_objects` 只要任何 config 或 CLI 啟用就是 true；匯入者的 false 不會關閉它。Input 與 output 仍明確寫在 CLI，不放在可重用 library 的設定裡。Output 不得覆蓋輸入、已登記的 header／library 或任何已讀取的 config。

設定只是 dependency metadata，不會執行腳本，也不會替 host 連結外部 library。DSL library 仍是編譯時檢查的 `.dsl.h` 原始碼；生成的 header bundle 供一般 C++ 使用，不是可匯入 DSL 的 binary。多份 bundle 包含同名 object 時，應合併 DSL 來源一次生成，避免重複定義。

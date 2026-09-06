# 數學運算、比較與條件選值

另有 `--emit-objects` 模式，提供外部 event、context binding、function object 與 DSL 原始碼 library；其型別與呼叫規則見 [function-objects.md](function-objects.md)。下文主要描述既有 scalar 模式。

DSL 現在支援四則運算、一元正負號、13 個數學函數、6 個比較運算，以及 `?:` 條件選值。Compiler、輸入解析及生成結果都使用 C++23。數學 API 現在由實體 runtime library 提供；多函數與連結方式見 [runtime-and-functions.md](runtime-and-functions.md)。

## 寫法

```cpp
#include <dsl_runtime/math.h>

double compute(double a, double b) {
    const double radius = dsl_math::sqrt(dsl_math::pow(a, 2.0) + dsl_math::pow(b, 2.0));
    const bool nonzero = radius > 0.0;
    return nonzero ? dsl_math::min(radius, 10.0) : 0.0;
}
```

這份範例保存在 [examples/math.dsl.cpp](../examples/math.dsl.cpp)。執行方式：

```sh
build/dslc examples/math.dsl.cpp --dump-ir -o build/math.cpp
g++ -std=c++23 -O2 -fno-fast-math -ffp-contract=off \
  -Iruntime/include build/math.cpp examples/math_driver.cpp \
  build/runtime/libdsl_runtime.a -o build/math
build/math
# 5
```

參數與回傳型別維持 `double`。區域變數可使用已初始化的 `const double` 或 `const bool`；`bool` 用來保存比較或條件運算的結果。可以使用 `true`、`false`。

## 支援的數學函數

所有數學函數的參數與回傳值都是 `double`；名稱區分大小寫，輸入使用表中的 DSL 名稱。

| DSL／生成程式呼叫的 API | Runtime 內部目前使用 | 意義 |
| --- | --- | --- |
| `dsl_math::abs(x)` | `std::abs(x)` | 絕對值。 |
| `dsl_math::sqrt(x)` | `std::sqrt(x)` | 平方根。 |
| `dsl_math::pow(x, y)` | `std::pow(x, y)` | x 的 y 次方。 |
| `dsl_math::exp(x)` | `std::exp(x)` | 自然指數。 |
| `dsl_math::log(x)` | `std::log(x)` | 自然對數。 |
| `dsl_math::sin(x)` | `std::sin(x)` | 正弦，輸入以弧度表示。 |
| `dsl_math::cos(x)` | `std::cos(x)` | 餘弦，輸入以弧度表示。 |
| `dsl_math::tan(x)` | `std::tan(x)` | 正切，輸入以弧度表示。 |
| `dsl_math::min(x, y)` | `std::fmin(x, y)` | 浮點最小值。 |
| `dsl_math::max(x, y)` | `std::fmax(x, y)` | 浮點最大值。 |
| `dsl_math::floor(x)` | `std::floor(x)` | 向負無限大方向取整，結果仍為 double。 |
| `dsl_math::ceil(x)` | `std::ceil(x)` | 向正無限大方向取整，結果仍為 double。 |
| `dsl_math::round(x)` | `std::round(x)` | 取最近整數；恰好一半時遠離零，例如 ±2.5 → ±3.0。 |

可以任意巢狀組合，例如 `dsl_math::sqrt(dsl_math::pow(dsl_math::abs(a), 2.0) + dsl_math::pow(b, 2.0))`。一元 `+a`、`-a` 與負數常數 `-1.0` 也已支援，`-0.0` 保留負零。

`min`／`max` 明確使用 `<cmath>` 的 `fmin`／`fmax`，不是 `<algorithm>` 的同名 template。一般 quiet NaN 輸入若只有一個運算元是 NaN，會選擇另一個數值；兩者都是 NaN 時結果為 NaN。相等值的 signed-zero 選擇沿用後端實作，不另加規則。

## 比較與條件

比較支援 `a < b`、`a <= b`、`a > b`、`a >= b`、`a == b`、`a != b`。兩個 operand 都必須是 double，結果為 bool。

```cpp
const bool positive = a >= 0.0;
const double root = positive ? dsl_math::sqrt(a) : 0.0;
```

`?:` 的條件必須是 bool，兩個分支必須是相同型別的 double 或 bool。可以巢狀使用，也可以把選值放在函數參數中：

```cpp
const double magnitude = dsl_math::sqrt(a < 0.0 ? -a : a);
const bool enabled = a < b ? true : false;
return enabled ? magnitude : b;
```

**只會執行被選中的分支。** 例如 `a >= 0.0 ? dsl_math::sqrt(a) : 0.0` 在 a 為負數時，不會呼叫 sqrt。條件本身只評估一次。

這裡仍不接受 `if`、迴圈、賦值、`&&`、`||` 或 `!`。多層判斷可用巢狀 `?:` 表達；函數本體依然是區域宣告加最後唯一的 return。

## 型別與錯誤邊界

沒有放寬隱式數值轉型：

| 輸入 | 結果 |
| --- | --- |
| `dsl_math::pow(a, 2.0)` | 接受。 |
| `dsl_math::pow(a, 2)` | 拒絕整數轉 double。 |
| `dsl_math::sqrt(true)` | 拒絕 bool 轉 double。 |
| `a ? b : c` | 拒絕 double 轉 bool；請寫明比較條件。 |
| `a > b ? a : 0` | 拒絕分支中的 int 轉 double；請寫 `0.0`。 |
| `return a > b;` | 拒絕 bool 轉 double。 |
| `const double x = a > b;` | 拒絕 bool 轉 double。 |
| `dsl_math::sqrt(a, b)`、`dsl_math::pow(a)` | 拒絕錯誤參數數量。 |
| `cot(a)` | 拒絕未宣告的函數呼叫。 |

數學函數的定義域是執行期行為。例如 `dsl_math::sqrt(-1.0)` 不在編譯期報 DSL 語法錯誤；runtime 沿用 `std::sqrt` 的行為。`dsl_math::log(0.0)`、overflow、NaN、Inf、errno 與浮點例外都交給後端數學函式庫，沒有改成例外、截斷或自動 clamp。

本次在 Linux x86_64 驗證了 sqrt 的 NaN、log 的負無限大、pow 的定義域錯誤，以及 NaN 比較行為。跨平台的最後位元、errno 與浮點環境細節，仍依後端實作及編譯選項而定。

## Compiler 如何處理這些功能

### 實體 library 與呼叫辨識

使用者明確 include [runtime/include/dsl_runtime/math.h](../runtime/include/dsl_runtime/math.h)，並以 `dsl_math::` 呼叫。實作位於 [runtime/src/math.cpp](../runtime/src/math.cpp)，建成 `dsl_runtime` library，生成程式需連結它。

[include/dsl/math.h](../include/dsl/math.h) 的 `mathBuiltins` 保存 compiler 使用的 API metadata。Frontend 檢查 include 真正解析到配置的實體 header，再驗證其中的 namespace、函數簽名與 canonical declaration identity。沒有虛擬標頭或隱含宣告。

同一份輸入內的 DSL helper 也可以呼叫；函數表先建立，body 再逐一 lower。`CallTarget` 區分 runtime API、DSL function ID 與 external function ID；外部介面見 [external-functions.md](external-functions.md)。參考 [runtime-and-functions.md](runtime-and-functions.md)。

### Typed IR 與分支

IR 增加了 `Type::Bool`、`BooleanConstant`、`Unary`、`Call`、`Select`，`BinaryOp` 增加六種比較。`Call` 保存 runtime enum 或 DSL function ID，以及已 lower 的 argument value IDs，不保存來源呼叫文字。

原先函數只有一個平面的執行序列；現在用 `Region` 記錄某個作用域要執行的 instruction IDs，以及其結果 ID。`Function::body` 是頂層 region，每個 `Select` 擁有 true／false 兩個 region。

例如 `a >= 0.0 ? dsl_math::sqrt(a) : 0.0` 的結構為：

```text
%0 : double = param a
%1 : double = constant 0x0p+0
%2 : bool = ge %0, %1
%5 : double = select %2 {
  then {
    %3 : double = call @dsl_math::sqrt(%0)
    yield %3
  }
  else {
    %4 : double = constant 0x0p+0
    yield %4
  }
}
return %5
```

每個函數的所有值仍存放在自己的 values 表，但 codegen 依 region 遍歷，不再把整張表從頭到尾無條件執行。分支中的值只在該分支內生成；對外使用的是 `Select` 的結果。

### Codegen

`Call` 生成實體 `dsl_math::` API 呼叫或 DSL helper 呼叫，比較生成 bool 暫存值。`Select` 生成立即呼叫的 lambda，內部用 if／else 執行被選中的 region 並回傳結果：

```cpp
const double v5 = [&]() -> double {
    if (v2) {
        const double v3 = dsl_math::sqrt(v0);
        return v3;
    } else {
        const double v4 = 0x0p+0;
        return v4;
    }
}();
```

這個 lambda 是從結構化 IR 生成的 C++，不是開放使用者在 DSL 裡寫 lambda 或 if。仍需使用 `-fno-fast-math -ffp-contract=off`，並遵守原有浮點環境前提。

## 驗證方式

新增測試涵蓋每個數學函數、比較、一元正負號、bool 綁定、巢狀選值、分支結果重用，以及錯誤型別／名稱／參數數量／宣告冒用的拒絕。

條件選值另以四組程式檢查 `FE_INVALID`、`FE_DIVBYZERO` 與 errno，確認未選中的 sqrt、log、除法及巢狀分支沒有執行。NaN／Inf 另有執行測試。完整最近一次結果見 [validation.md](validation.md)。

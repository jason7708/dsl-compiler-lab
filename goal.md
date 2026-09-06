我要在這個 repo 製作一個獨立的 DSL compiler 原型，請直接實作，完成建置和執行驗證。

背景：
未來 DSL 會用來組合計算 OP，生成 C++ kernel，再由外部程式接到不同框架。這次只驗證 compiler 核心。

技術方向：

* 使用 C++、CMake，以及 Clang LibTooling。
* DSL 是合法 C++ 的受限子集。
* 流程是：Clang AST → DSL 規則檢查 → 自訂 typed IR → C++ codegen。
* 生成的 C++ 再交給一般 C++ compiler 編譯。
* 固定並記錄實際使用的 LLVM／Clang 版本。

第一版只接受：

* 一個名為 compute 的函數。
* 參數和回傳型別都是 double。
* double 常數、參數引用、初始化後不再修改的 const double 區域變數。
* 二元 +、-、*、/ 和括號。
* 函數最後的一個 return。
* 其他使用者語法一律明確拒絕，回報原始碼位置與原因。

驗收範例：
double compute(double a, double b) {
const double sum = a + b;
return sum * 0.5;
}

預期提供：

1. CLI，能讀取輸入檔、顯示 IR，並將生成的 C++ 寫到指定檔案。
2. 最小 IR，包含函數參數、型別、常數、運算、值引用與 return。
3. 可編譯執行的生成結果。
4. 測試運算優先序、括號、區域變數引用，以及不支援語法的拒絕行為。
5. README，包含依賴、建置方式、執行範例，以及各模組的責任。

限制：

* 不自行撰寫 C++ parser。
* 不透過直接複製輸入原始碼冒充 codegen；輸出必須由自訂 IR 生成。
* 暫不加入 LLVM IR、MLIR、JIT、Python binding、FPGA、框架整合或最佳化。
* 保留浮點運算順序。
* 避免為未來功能建立大量抽象層。

工作方式：
先檢查目前環境與 repo，簡短說明實作安排，再開始動手。環境允許時完成端到端驗證；缺少依賴時明確說明，不要把未執行的測試當成通過。

完成後，用驗收範例解釋 AST 如何轉成 IR，以及 IR 如何生成 C++，並指出我應該先閱讀哪幾個檔案。

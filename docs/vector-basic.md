# vector-basic 代码实现与原理

分支：`submit-vector-basic`。本文对应实现提交：`fb2a2e85a1861ceed0085e392cc41e49b34973cf`。

这个实验让数据库支持三件事：保存浮点数组、在 SQL 中转换数组、计算两个数组的距离。原工程已经负责接收 SQL、扫描表和写入记录，本次把向量接进这些流程。

从这组语句开始：

```sql
CREATE TABLE TEST (ID INT, C1 VECTOR(3));
INSERT INTO TEST VALUES (1, STRING_TO_VECTOR('[1,2,3]'));
SELECT ID, VECTOR_TO_STRING(C1) AS VEC_STR FROM TEST;
```

## 1. 建表：记录每列的类型和容量

表里的每行都有 `ID` 和 `C1` 两个字段，可以理解为结构相同的一组 C++ 对象。数据库另外保存列名、类型、长度等信息，称为元数据，用于解释记录中的字节。

解析 `C1 VECTOR(3)` 时，语法规则把它记为：

```cpp
// 表示元数据中的类型和容量。
type   = AttrType::VECTORS;
length = 3 * sizeof(float);  // 允许最多保存 12 字节向量数据
```

本题 `VECTOR(N)` 的 `N` 是最大元素数量；省略时为 2048，上限为 16383。`VECTOR()` 缺少数字，属于语法错误。

相关代码：[yacc_sql.y 的 attr_def 规则](../src/observer/sql/parser/yacc_sql.y)。

## 2. 插入：先把字符串解析成浮点数组

执行 `STRING_TO_VECTOR('[1,2,3]')` 时，解析器创建一个 `VectorFunctionExpr` 对象，表示这次函数调用，参数是字符串常量。

INSERT 的 `value_list` 语法规则调用它的 `try_get_value()`。这个函数的含义是：不读取表中的行，能否直接算出值？这里参数全是常量，所以可以。

随后由函数求值逻辑调用：

```cpp
VectorType::set_value_from_str(value, "[1,2,3]");
```

这个函数依次检查左括号、数字、逗号和右括号，用 `strtof()` 将数字转换为 `float`。缺括号、缺数字、末尾多逗号、出现无穷值等都会失败；十进制和科学记数法都可以解析。

解析成功后，`Value::set_vector()` 把三个浮点数复制到自己管理的内存中：

| 内容 | 本例的值 |
| --- | --- |
| 类型 | `VECTORS` |
| 数据 | 三个连续的 `float`：`1.0f`、`2.0f`、`3.0f` |
| 长度 | 12 字节 |

`Value` 是原工程用来传递一个带类型的值的容器。新增向量支持后，复制它时也要复制数组内容，销毁时释放数组，避免临时数组失效后留下悬空指针。

相关代码：[VectorFunctionExpr](../src/observer/sql/expr/vector_function_expr.cpp)、[VectorType::set_value_from_str()](../src/observer/common/type/vector_type.cpp)、[Value::set_vector()](../src/observer/common/value.cpp)。

## 3. 写入磁盘：记录中保存位置，数组写进独立文件

原有 `InsertPhysicalOperator::open()` 调用 `Table::make_record()`，把各个值编码成一条记录。本次在 `Table::set_value_to_record()` 中增加了向量分支。

原工程一个数据页只有 8192 字节，而最大向量需要：

```text
16383 × 4 = 65532 字节
```

因此代码复用原有的 `LobFileHandler`，把数组追加写入独立的向量文件；主记录中的 `C1` 保存两个整数：

```cpp
int64_t reference[2];
// reference[0]：数组在文件中的起始偏移
// reference[1]：数组实际占用的字节数
```

假设 `[1,2,3]` 写在文件开头，主记录就保存 `{0, 12}`。以后读取该字段，便从文件偏移 0 读取 12 字节。这里保存的是文件位置，重启后仍然有效。

写入时，代码先检查实际向量长度没有超过列容量，再调用 `insert_data()` 写入数据并取得偏移，最后把偏移和长度复制进主记录。

这里必须区分两种长度：

- `FieldMeta::len()`：这一列允许保存多长的向量，本例为 12 字节。
- `FieldMeta::storage_len()`：这一列在主记录中占多少空间，向量固定为 16 字节。

计算下一列的位置时使用 `storage_len()`，否则后面的字段可能覆盖向量引用。建表时创建向量文件，重新打开表时打开同一文件，查询才能在重启后恢复数组。

相关代码：[Table::set_value_to_record()](../src/observer/storage/table/table.cpp)、[FieldMeta::storage_len()](../src/observer/storage/field/field_meta.h)、[TableMeta 的字段布局](../src/observer/storage/table/table_meta.cpp)。

## 4. 查询：找到列，恢复数组，再执行函数

执行：

```sql
SELECT ID, VECTOR_TO_STRING(C1) AS VEC_STR FROM TEST;
```

解析器最初只知道参数名字叫 `C1`。`ExpressionBinder` 查找表结构，把这个名字绑定到真实字段，并检查它确实是向量类型。

查询扫描到每行时，依次发生：

1. `VectorFunctionExpr::get_value()` 请求参数 `C1` 的值。
2. `FieldExpr::get_value()` 从当前行查找字段。
3. `RowTuple::cell_at()` 发现这是向量，调用 `Table::read_vector()`。
4. `read_vector()` 读取记录中的偏移和长度，从向量文件恢复数组。
5. `VectorType::to_string()` 把数组格式化为字符串。

这里的 `get_value()` 与前面的 `try_get_value()` 有区别：`C1` 每行可能不同，必须拿到当前行才能计算。两条路径最终共用 `VectorFunctionExpr::evaluate()` 中的求值逻辑。

格式化使用：

```cpp
std::scientific << std::setprecision(5)
```

即科学记数法，小数点后保留五位，所以输出为：

```text
ID | VEC_STR
1 | [1.00000e+00,2.00000e+00,3.00000e+00]
```

`AS VEC_STR` 只设置结果列名称。先前评测失败是因为缺少 `expression AS ID` 语法规则；查询读到 `AS` 时就失败，尚未读取记录。修复后解析器保存别名，绑定普通字段时也保留别名，原有输出代码读取这个名称作为表头。

相关代码：[ExpressionBinder](../src/observer/sql/parser/expression_binder.cpp)、[RowTuple::cell_at()](../src/observer/sql/expr/tuple.h)、[Table::read_vector()](../src/observer/storage/table/table.cpp)。

## 5. 距离计算：先取到两个数组，再遍历元素

例如：

```sql
SELECT DISTANCE(C1, TO_VECTOR('[4,6,3]'), 'EUCLIDEAN') FROM TEST;
```

`VectorFunctionExpr` 先取得当前行的 `C1`，再把常量字符串转换成另一个向量，最后进入 `evaluate()`。

核心循环如下，省略了循环前的参数检查：

```cpp
double dot = 0, square_a = 0, square_b = 0, square_diff = 0;
for (int i = 0; i < a.vector_dimension(); ++i) {
    double x = a.vector_at(i);
    double y = b.vector_at(i);

    dot         += x * y;
    square_a    += x * x;
    square_b    += y * y;
    square_diff += (x - y) * (x - y);
}
```

随后根据方法名返回：

| 方法 | 计算方式 | `[1,2,3]` 与 `[4,6,3]` 的结果 |
| --- | --- | --- |
| `EUCLIDEAN` | `sqrt(square_diff)` | `5` |
| `DOT` | `dot` | `25` |
| `COSINE` | `1 - dot / (sqrt(square_a) * sqrt(square_b))` | `1 - 25 / sqrt(14 * 61)` |

元素存成 `float`，中间用 `double` 累加，最终返回 `float`。单次计算遍历一次数组，时间复杂度为 O(d)，其中 d 是实际维度。

`L2_DISTANCE`、`INNER_PRODUCT`、`COSINE_DISTANCE` 分别直接进入这三种计算；`TO_VECTOR`、`FROM_VECTOR`、`VECTOR_DISTANCE` 则在构造函数中统一为对应函数名。

相关代码：[VectorFunctionExpr::evaluate()](../src/observer/sql/expr/vector_function_expr.cpp)。

## 6. 错误处理：整条查询失败时统一返回 FAILURE

代码用 `RC` 表示成功或错误。维度不同、参数类型错误、方法名非法、余弦计算遇到零向量，都会返回错误，最终向客户端输出 `FAILURE`。

逐行计算时，前两行可能合法，第三行才出现维度错误。因此 `SqlResult::open()` 对标记为涉及向量的查询先计算并缓存全部结果，全部成功后才发送。这样不会已经发出几行数据，后面才发现整条查询失败；代价是结果集需要额外内存。

相关代码：[SqlResult::open()](../src/observer/sql/executor/sql_result.cpp)。

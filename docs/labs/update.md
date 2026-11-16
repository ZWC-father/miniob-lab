# UPDATE 实验：原理与代码实现

当前合并分支：`submit`；原独立分支：`submit-update`。原实现提交为 [`776839d08d9a252eeec154a1b5f422bfb401681c`](https://github.com/ZWC-father/miniob-lab/commit/776839d08d9a252eeec154a1b5f422bfb401681c)。

## 1. 这个实验要完成什么

数据库的表由行和列组成：一行是一条记录，一列是一个字段。例如 `student(id int, age int)` 中，`(2,20)` 表示一条 `id=2`、`age=20` 的记录。

```sql
UPDATE student SET age=100 WHERE id=2;
```

这条 SQL 的含义是：在 `student` 表中，找出所有 `id=2` 的记录，把它们的 `age` 改成 `100`。其他字段和不满足条件的记录保持原值。

| SQL 部分 | 含义 |
| --- | --- |
| `UPDATE student` | 指定要修改的表 |
| `SET age=100` | 指定目标字段及新值 |
| `WHERE id=2` | 指定哪些行需要修改 |

没有 WHERE 就更新全表；条件没有匹配记录时，合法的 UPDATE 返回成功，但不改变数据。

原工程已经能解析 UPDATE 语法，但 `UpdateStmt::create()` 还是 TODO，语句分发、执行计划和具体更新操作也没有接齐。本次实现把这些环节连接起来。

## 2. 解析 SQL：提取表名、字段、新值和条件

位置：[yacc_sql.y](../../src/observer/sql/parser/yacc_sql.y) 的 `update_stmt` 规则；数据结构：[parse_defs.h](../../src/observer/sql/parser/parse_defs.h) 的 `UpdateSqlNode`。

原有语法规则识别下面的结构：

```text
UPDATE 表名 SET 字段名 = 常量 WHERE条件
```

解析结果保存在 `UpdateSqlNode` 中：

| 成员 | 示例中的内容 |
| --- | --- |
| `relation_name` | `student` |
| `attribute_name` | `age` |
| `value` | 整数 `100` |
| `conditions` | 条件 `id=2` |

这一步只是把文本转换成结构化数据，还没有查表或修改记录。独立实现曾在 `value` 规则补负数字面量；合并后统一使用原有表达式规则，再调用 `try_get_value()` 求出常量，消除了重复负数规则产生的 36 处解析冲突。`SET age=-7` 和 `SET v=TO_VECTOR('[1,2]')` 都能得到不依赖记录的值；`SET age=age+1` 依赖字段，仍然拒绝。求值后释放临时表达式。条件中的负数同样走一元负号表达式，绑定时只要求左操作数，不再错误要求右操作数。

## 3. 检查语义：确认这次更新合法

调用入口是 [stmt.cpp](../../src/observer/sql/stmt/stmt.cpp) 的 `Stmt::create_stmt()`。它遇到 `SCF_UPDATE` 后，调用 [update_stmt.cpp](../../src/observer/sql/stmt/update_stmt.cpp) 的 `UpdateStmt::create()`。

`UpdateStmt::create()` 按顺序进行以下检查：

1. 调用 `db->find_table()` 查找目标表，不存在则返回错误。
2. 在用户字段中查找目标字段，不存在或试图修改系统字段则返回错误。
3. 检查新值的类型。类型不同时调用原框架的 `Value::cast_to()`，转换不支持或失败就返回错误。
4. 若目标是向量，检查实际元素数据长度不能超过字段容量；空表和无匹配条件也不能绕过校验。
5. 调用原有的 `FilterStmt::create()`，检查 WHERE 中引用的表、字段，并建立过滤条件。

通过后生成 `UpdateStmt`，保存目标表、字段元信息、新值和过滤条件。字段元信息包括类型、在记录中的字节偏移、占用长度等，后面修改记录时需要使用。

类型检查在扫描之前完成。因此，即使条件没有匹配任何行，下面的非法赋值也会失败：

```sql
UPDATE student SET age='abc' WHERE id=999;
```

否则，错误语句是否报错就会取决于表里碰巧有没有匹配行。

## 4. 生成计划：安排查找、过滤和更新

[logical_plan_generator.cpp](../../src/observer/sql/optimizer/logical_plan_generator.cpp) 中新增的 `LogicalPlanGenerator::create_plan(UpdateStmt *)` 将任务拆为三个操作：

| 逻辑算子 | 作用 | 来源 |
| --- | --- | --- |
| `TableGetLogicalOperator` | 获取目标表的记录，使用读写模式 | 复用原框架 |
| `PredicateLogicalOperator` | 用 WHERE 条件过滤记录；无条件时不需要这一层 | 复用原框架 |
| `UpdateLogicalOperator` | 保存目标表、字段和新值，接收待更新记录 | 本次新增 |

逻辑计划描述要做哪些操作。随后，[physical_plan_generator.cpp](../../src/observer/sql/optimizer/physical_plan_generator.cpp) 中的 `PhysicalPlanGenerator::create_plan(UpdateLogicalOperator &)` 将其转换成实际执行的对象，包括扫描算子和本次新增的 `UpdatePhysicalOperator`。

“算子”可以理解成一个处理记录的 C++ 对象：`open()` 开始处理，`next()` 获取下一条结果，`close()` 结束处理。一个算子通过调用子算子获取输入记录。

原有优化器可以把过滤条件下推给扫描算子。存在可用的等值索引时，例如 `id=2`，可以通过索引查找；否则扫描表并判断条件。

本次还修正了扫描计划中的一个错误：`NOT_EQUAL` 曾被当作等值条件，用于建立左右边界相同的索引扫描。对于 `id<>2`，只扫描索引键 `2` 会漏掉应更新的记录，因此现在只有 `EQUAL_TO` 可以进入这条等值索引查找路径。

UPDATE 被标记为不能生成向量化算子，执行时走已经实现的逐记录路径，即使会话选择了 `CHUNK_ITERATOR` 也一样。

## 5. 执行更新：先收集记录，再构造新记录

核心函数是 [update_physical_operator.cpp](../../src/observer/sql/operator/update_physical_operator.cpp) 的 `UpdatePhysicalOperator::open()`。

### 5.1 收集全部匹配记录

它先打开子算子，反复调用子算子的 `next()`，把每条匹配记录深拷贝到 `vector<Record>`，并保留记录的 `RID` 和 key。全部读取成功后关闭扫描器，再开始修改。

`RID` 是记录的位置标识，包含页号和槽位号，后面删除旧记录时需要使用。

这里必须复制实际数据，因为扫描返回的 `Record` 可能只是引用数据库页面内存。只保存指针，继续扫描后就不能保证仍然拿着原来的记录内容。

先收集再更新，也避免了边扫描边改变索引内容。例如修改正在用于查找的索引字段时，记录在索引中的位置可能改变；一边遍历一边修改可能导致漏行、重复处理或锁冲突。

扫描异常会直接返回错误，不会把异常当成正常结束。只有 `RECORD_EOF` 才表示匹配记录已经读完。若优化器因恒假条件消除了子计划，则直接成功返回，表示没有记录需要更新。

### 5.2 修改目标字段的字节

对每条旧记录，先复制出新记录：

```cpp
Record new_record = old_record;
char *dest = new_record.data() + field_.offset();
```

一条记录在内存中是一段字节。`field_.offset()` 是字段起点；`storage_len()` 是行内占用字节数。向量的 `len()` 是最大元素容量，行内实际只存两个 64 位整数，即 LOB 偏移和实际数据长度，不能用容量覆盖行内数据。

然后清空目标字段，并写入新值：

```cpp
memset(dest, 0, field_.storage_len());
rc = table_->set_value_to_record(new_record.data(), value_, &field_);
```

这里与 INSERT 共用 `Table::set_value_to_record()`：普通字段直接编码，向量先追加到 `.data.vector`，再把新的偏移和长度写进行内。写入失败立即返回。其他字段保持原值，包括相邻字段和未更新的向量引用。

`Value` 复制空字符串时也保留结尾的 NUL，避免共用编码函数时读取空指针。先清空是为了正确处理短字符串覆盖长字符串。例如将 `"abcdef"` 改成 `"x"` 时，不能留下原字符串后面的字节。

最后调用事务接口：

```cpp
trx->update_record(table_, old_record, new_record);
```

## 6. 写入存储：数据和索引一起维护

### 6.1 默认 Vacuous 模式

[vacuous_trx.cpp](../../src/observer/storage/trx/vacuous_trx.cpp) 的 `VacuousTrx::update_record()` 采用以下顺序：

```cpp
RC rc = table->insert_record(new_record);
// 插入成功后：
rc = table->delete_record(old_record);
```

先插入新记录，成功后再删除旧记录。插入失败时保留旧记录；删除失败时，尝试删除刚插入的新记录。

这样实现可以复用原框架维护索引的代码。在 [heap_table_engine.cpp](../../src/observer/storage/table/heap_table_engine.cpp) 中，`HeapTableEngine::insert_record()` 会插入记录并建立索引项，`HeapTableEngine::delete_record()` 会删除索引项及旧记录。

索引保存字段值与记录位置的对应关系。例如，`id` 索引中的键值 `2` 对应一个记录位置 `RID`。更新后不仅可能改变键值，也可能改变记录位置，所以必须同步修改索引。即使更新的是 `age`，没有改变 `id`，新记录的位置变了，`id` 索引也仍然需要维护。

这里的“先插入、再删除”发生在数据库存储层，不是另外执行两条 SQL。

### 6.2 MVCC 模式

[mvcc_trx.cpp](../../src/observer/storage/trx/mvcc_trx.cpp) 的 `MvccTrx::update_record()` 调用原有事务删除和插入接口：

```cpp
RC rc = delete_record(table, old_record);
if (rc == RC::SUCCESS) {
  rc = insert_record(table, new_record);
}
```

MVCC 使用多个记录版本管理事务看到的数据。这里的删除先将旧版本标记为删除，再插入新版本，沿用原有版本可见性判断、事务操作记录和日志机制处理提交与回滚。

合并回归还修正了 `MvccTrx::rollback()`：撤销插入的新版本前，先通过 RID 读取完整记录，再删除记录和索引。原来只传 RID，索引删除读不到字段数据，会在有索引的更新回滚时崩溃。

合并删表检查点逻辑后，数据库元数据也保存并恢复最大事务编号；日志回放遇到提交记录时继续推进编号，保证重启后能看到最新的已提交版本。

本次还修改了 `MvccTrx::visit_record()`：当前事务自己插入、随后又删除的版本，不能再次被当前事务读到。否则，同一事务中连续更新可能把已经淘汰的版本也当成有效记录处理。

## 7. 返回结果：避免更新失败后误报成功

UPDATE 的实际工作在 `UpdatePhysicalOperator::open()` 中完成，不向客户端返回查询行。因此 `next()` 直接返回 `RECORD_EOF`，`close()` 返回保存的执行结果 `result_`。

原框架的 [SqlResult::close()](../../src/observer/sql/executor/sql_result.cpp) 根据算子的关闭结果处理自动事务：成功时提交，失败时调用回滚。如果更新失败，但 `close()` 错误地返回成功，就会进入错误的提交路径，所以执行错误必须保存并传回。

需要区分：默认 `VacuousTrx` 没有完整事务回滚能力；MVCC 才通过事务版本和操作记录进行回滚。不能把 Vacuous 模式下的局部失败处理理解为整条语句具备完整事务原子性。

## 8. 对照源码的阅读顺序

以下路径都相对于 `src/observer/`：

| 顺序 | 文件或函数 | 本次作用 |
| --- | --- | --- |
| 1 | `sql/parser/yacc_sql.y`、原有 `UpdateSqlNode` | 解析表、字段、常量和条件；统一常量表达式求值及临时对象释放 |
| 2 | `sql/stmt/stmt.cpp`、`sql/stmt/update_stmt.h/.cpp` | 增加语句分发，完成元数据、类型和条件校验 |
| 3 | `sql/optimizer/logical_plan_generator.h/.cpp`、新增 `update_logical_operator.h` | 连接获取记录、过滤、更新三个逻辑操作 |
| 4 | `sql/optimizer/physical_plan_generator.h/.cpp` | 生成更新执行对象；修正不等条件误用等值索引的问题 |
| 5 | 新增 `sql/operator/update_physical_operator.h/.cpp` | 收集记录、构造新记录、调用事务更新并返回结果 |
| 6 | `storage/trx/vacuous_trx.h/.cpp`、`mvcc_trx.h/.cpp` | 实现两种事务模式下的更新接口及必要的版本可见性处理 |
| 7 | `sql/operator/logical_operator.h/.cpp`、`physical_operator.h/.cpp` | 补 UPDATE 类型标识、逐记录执行选择和执行计划名称 |

表扫描、条件过滤、索引结构、记录插入删除以及 `SqlResult` 的事务收尾是原框架已有能力；本次主要增加 UPDATE 的检查与执行链路，并复用这些能力。

回归用例位于 [test/case/test/update.test](../../test/case/test/update.test)，对应期望结果位于 [test/case/result/update.result](../../test/case/result/update.result)。

## 9. 原独立分支的验证与实现范围

- 默认 Vacuous 模式下，basic 回归、56 条定向 SQL、1,065 条随机 SQL 检查通过，随机数据在正常关闭重启后也核对一致。
- 定向检查覆盖条件更新、全表更新、索引字段更新、无匹配、非法表和字段、非法赋值、负数、字符串覆盖及执行模式切换。
- MVCC 另做了连续更新、回滚和正常重启后的结果核对；较大的 MVCC 随机压力测试在 Debug/ASan 环境中达到 90 秒超时，不能据此宣称大规模性能验证通过。
- 平台隐藏测试未运行。

本次支持单字段赋常量，例如 `SET age=100`。`SET age=age+1`、一次修改多个字段不在本次实现范围内。匹配记录会暂存在内存中，内存占用随匹配行数和记录长度增长。

合并版另测日期索引更新、向量更新的长度校验、相邻字段完整性、重复更新和事务回滚。具体运行命令与结果见 [README.md](README.md)。

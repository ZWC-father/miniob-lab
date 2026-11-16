# DROP TABLE 实验：原理与代码实现

当前合并分支：`submit`；原独立分支：`submit-drop-table`。原实现提交：`3a67cee9a1542ef4490f5bb3dfdd09cbc0531cc9`。

这个实验实现 `DROP TABLE t;`：删除表的定义、全部记录、索引和内存中的管理对象。删除成功后，再访问这张表应返回失败，同名表可以重新创建。

**先看一张表由什么组成。**

假设执行：

```sql
CREATE TABLE t(id int, age int);
CREATE INDEX age_idx ON t(age);
```

MiniOB 会管理以下资源：

| 对象 | 保存什么 |
| --- | --- |
| `t.table` | 表名、列名、字段类型、索引清单。这些描述信息叫“元数据” |
| `t.data` | 实际插入的各行记录 |
| `t-age_idx.index` | `age` 值与记录位置的对应关系，用于加快查找 |
| `opened_tables_["t"]` | 内存中的 `Table*`，供 SQL 执行时查找这张表 |
| 缓冲池和文件句柄 | 缓存在内存中的数据页，以及操作系统打开的文件 |

因此，删除表需要同时清理磁盘文件和内存对象。

**一条删表 SQL 的执行过程。**

1. 原有解析器识别命令。`yacc_sql.y` 已经能把 `DROP TABLE t;` 解析成 `SCF_DROP_TABLE`，并保存表名 `t`。这部分没有修改。
2. `Stmt::create_stmt()` 进入新增的删表分支，调用 `DropTableStmt::create()`。它检查数据库、表名和表是否存在；通过后创建一个保存表名的语句对象。`Stmt` 表示已经完成校验的命令。
3. `CommandExecutor::execute()` 根据语句类型 `DROP_TABLE`，调用新增的 `DropTableExecutor::execute()`。
4. 执行器从当前会话取得数据库，再调用 `Db::drop_table(table_name)`，完成实际删除。
5. 返回码沿原框架传回客户端。成功显示 `SUCCESS`，失败显示 `FAILURE`。原框架还会在成功执行 DDL 后同步数据库；DDL 指建表、删表这类修改数据库结构的操作。

执行器的核心调用是：

```cpp
auto *drop_table_stmt = static_cast<DropTableStmt *>(stmt);
return sql_event->session_event()->session()->get_current_db()->drop_table(
    drop_table_stmt->table_name().c_str());
```

**真正的删除发生在 `Db::drop_table()`。**

代码位置：[src/observer/storage/db/db.cpp](../../src/observer/storage/db/db.cpp)。它按以下顺序操作：

1. 检查表名，并从 `opened_tables_` 查找 `Table` 对象。表不存在时返回 `SCHEMA_TABLE_NOT_EXIST`。
2. 在对象销毁前收集所有文件路径：遍历 `meta.index_num()` 得到每个索引的路径，再加入数据文件、向量使用的 `.data.vector` 文件、可选的 `.lob` 文件和 `.table.tmp` 临时文件。
3. 调用 `Db::sync()`，同步数据并推进数据库的日志检查点。同步失败就停止删除。
4. 用 `unlink()` 删除 `.table` 元数据文件。MiniOB 启动时会扫描这类文件加载表；删除成功后，重启不会再加载这张表。删除元数据失败时立即返回，内存中的表仍然保留。
5. 用 `opened_tables_.erase(iter)` 从内存表目录移除条目，再执行 `delete table`。原有析构代码会释放记录处理器、索引和缓冲池，并关闭文件句柄。
6. 逐个删除先前收集的数据、索引等文件。文件已经不存在，即 `errno == ENOENT`，可以继续；其他删除错误记录下来并返回 `FILE_REMOVE`。

其中有三个容易混淆的操作：

| 操作 | 作用 |
| --- | --- |
| `opened_tables_.erase(iter)` | 从数据库的内存目录移除指针，不会自动销毁指针指向的对象 |
| `delete table` | C++ 对象析构，释放内存和打开的资源 |
| `::unlink(file.c_str())` | 删除文件系统中的文件名 |

必须在 `delete table` 之前保存文件路径，因为表名和索引元数据也会随对象一起销毁。索引则必须逐个枚举，因为它们可能是在建表之后陆续创建的。

**删除前为什么要同步，日志恢复为什么也要改？**

数据库会记录修改日志，重启时重做尚未同步的数据。日志顺序号叫 LSN；检查点表示截至哪个 LSN，数据已经完成同步。检查点覆盖整个数据库，因此这里调用 `Db::sync()` 同步所有表。

例如检查点是 `100`，而第 `100` 条日志引用了表 `t` 的索引。删除 `t` 后，如果重启仍然回放这条日志，就会查找已经删除的索引缓冲池，导致启动失败。

原框架的日志迭代包含起始 LSN，因此本次让 `Db::recover()` 把检查点传给 `IntegratedLogReplayer`，跳过已经同步的日志：

```cpp
if (check_point_lsn_ > 0 && entry.lsn() <= check_point_lsn_) {
  return RC::SUCCESS;
}
```

合并 UPDATE 后，检查点还要保存 MVCC 已分配的最大事务编号。数据库元数据现在保存 `LSN 事务编号`，启动时恢复编号；检查点之后的提交日志也推进编号。否则数据虽然仍在磁盘上，新事务编号从头开始，却会把已经提交的行判断为不可见。旧的单个 LSN 格式仍可读取。

检查点之后的日志仍然正常回放。这处理的是已经成功同步并删除的表在重启时被旧日志再次访问的问题，不代表删除过程具有完整的事务回滚能力。

**本次新增和修改的代码。**

下表路径均相对于仓库根目录；`.h/.cpp` 表示同名头文件和实现文件。

| 文件 | 函数或类 | 改动 |
| --- | --- | --- |
| `src/observer/sql/stmt/drop_table_stmt.h/.cpp` | `DropTableStmt`、`create()` | 新增语句类，校验表并保存表名 |
| `src/observer/sql/stmt/stmt.cpp` | `Stmt::create_stmt()` | 接入 `SCF_DROP_TABLE` 分支 |
| `src/observer/sql/executor/drop_table_executor.h/.cpp` | `DropTableExecutor::execute()` | 新增执行器，调用当前数据库的删表函数 |
| `src/observer/sql/executor/command_executor.cpp` | `CommandExecutor::execute()` | 接入 `DROP_TABLE` 执行分支 |
| `src/observer/storage/db/db.h/.cpp` | `Db::drop_table()` | 新增删表入口，清理内存对象和文件 |
| `src/observer/storage/db/db.cpp` | `Db::recover()` | 将数据库检查点交给日志回放器 |
| `src/observer/storage/db/db.cpp` | `Db::flush_meta()` | 补上遗漏的 `close(fd)`，避免每次同步元数据都泄漏文件句柄 |
| `src/observer/storage/clog/integrated_log_replayer.h/.cpp` | 构造函数、`replay()` | 接收检查点，跳过已经同步的日志 |
| `src/observer/storage/default/default_handler.cpp` | `DefaultHandler::drop_table()` | 将原有未实现入口接到 `Db::drop_table()` |
| `test/case/result/primary-drop-table.result` | 原有测试的预期结果 | 补充预期结果，原有测试脚本保持不变 |

表对象、存储引擎、缓冲池和索引的析构机制沿用原框架。本次利用这些机制释放资源，没有重新实现记录删除或索引结构。

**可以用下面的 SQL 检查基本行为。**

```sql
CREATE TABLE t(id int, age int);
INSERT INTO t VALUES(1, 20);
CREATE INDEX age_idx ON t(age);
DROP TABLE t;                       -- SUCCESS
SELECT * FROM t;                    -- FAILURE
DROP TABLE t;                       -- FAILURE
CREATE TABLE t(name char(20));      -- SUCCESS，可以使用新的表结构
```

原独立分支的本地验证包括原有 `primary-drop-table`、`basic`，以及 1014 条扩展 SQL 断言。默认模式和磁盘日志模式下，各完成了 30 轮双索引建删，检查文件和句柄清理、同名重建、重启，以及删表成功返回后立即终止进程再启动的行为；另有日志回归测试通过。这些结果不包含平台隐藏测试。

本实现沿用实验的无并发约束，删除时不能还有事务正在使用该表。若元数据已经删除，而后续资源文件删除失败，会返回失败并可能留下残余文件，不会自动恢复原表。

合并后新增了真实向量表的反复建删测试，确认 `.data.vector` 文件被删除，同名向量表可重新创建。合并版本的运行命令与结果见 [README.md](README.md)。

# 四个数据库实验的统一提交

提交分支：`submit`。四题代码在同一工程内，讲解集中保存在本目录。

| 实验 | 讲解 | 合入的原分支提交（含原讲解） |
| --- | --- | --- |
| date | [date.md](date.md) | `submit-date`：`96e2779efd9aea9f8ee8443af120d029cc572ef0` |
| vector-basic | [vector-basic.md](vector-basic.md) | `submit-vector-basic`：`13ced1283b47b2ebac105b57e1378df29a5bc56b` |
| update | [update.md](update.md) | `submit-update`：`0384f806689605f4278065c0edff426da10de1a2` |
| drop-table | [drop-table.md](drop-table.md) | `submit-drop-table`：`b420f7228c9ce5a1ce5b3913e8e383e815e1a57c` |

## 合并时修正的兼容问题

| 位置 | 问题与修正 |
| --- | --- |
| `sql/parser/yacc_sql.y` | 两个分支的负数规则重复，Bison 出现 36 处 reduce/reduce 冲突。UPDATE 改为共用表达式解析及常量求值，删除重复规则，最终零解析冲突。 |
| `sql/parser/expression_binder.cpp` | 一元负号只有左操作数，绑定时不再错误要求右操作数，恢复 `WHERE age=-7` 等条件。 |
| `common/value.cpp` | 空字符串复制也分配并保存结尾 NUL，避免 UPDATE 共用字段编码后访问空指针。 |
| `sql/stmt/update_stmt.cpp`、`sql/operator/update_physical_operator.cpp`、`storage/table/table.h` | 更新前校验向量容量；清空实际行内长度，并与 INSERT 共用字段编码。向量写入外置数据文件，行内只保存偏移和长度，不覆盖相邻字段。 |
| `storage/db/db.cpp` | DROP TABLE 同时删除真实向量文件 `.data.vector`，支持同名向量表重建。 |
| `storage/trx/mvcc_trx.cpp` | 回滚新版本前按 RID 取回完整记录，保证能正确删除索引项。 |
| `storage/db/db.cpp`、`storage/trx/trx.h`、`storage/trx/mvcc_trx.h/.cpp` | 检查点同时保存并恢复最大事务编号，回放后续提交也推进编号，避免重启后已提交行不可见或旧版本再次可见。 |

上表代码路径均相对于 `src/observer/`。原有的 DATE 类型转换、向量格式与别名、三种距离、UPDATE 索引维护、DROP TABLE 资源清理均保留。原始构建脚本、CLI 和评测脚本不变；只为原有 `primary-date`、`primary-update` 测试补充了独立核对的预期结果。

## 验证记录

2026-11-16，本地环境为 Debian 13、g++ 14.2.0、Debug + AddressSanitizer，关闭 MemTracer；ASan 检查内存访问错误，未启用泄漏检查。下列结果均针对合并后的代码。

| 检查 | 结果 |
| --- | --- |
| 完整构建、Bison 语法、差异格式检查 | 通过；无解析冲突、无遗留合并标记 |
| 原生 SQL 回归 | `basic`、`date`、`update`、`primary-date`、`primary-update`、`primary-drop-table` 六组通过 |
| vector-basic | 587 条 SQL 检查通过，含此前平台失败的 AS 别名及六行精确输出复现、随机距离、默认/最大维度、重启 |
| UPDATE 补充回归 | 56 条定向 + 1,065 条随机 + 1 条重启全表核对，共 1,122 条通过 |
| DROP TABLE 补充回归 | 1,014 条通过；默认/磁盘日志模式各 30 轮双索引建删，检查文件、句柄、重建和强制终止后恢复 |
| 合并专用回归 | 2,411 条通过；三种运行模式、日期与向量更新、索引、非法值、相邻字段、闰年、六种比较、回滚、检查点及强制终止后恢复 |
| 类型、解析、表达式、缓冲池和索引日志单测 | 6 个测试程序、35 个测试通过 |
| MVCC 日志恢复单测 | 开启 `CONCURRENCY=ON` 后，5 个测试通过，含异常中断恢复 |

补充 SQL 检查合计 5,134 条，不含原生回归重复执行的语句。并发日志单测启动四个线程，必须启用 `CONCURRENCY=ON`；默认构建关闭存储锁，不能用该构建判断并发单测结果。普通四题回归使用默认的无并发模式。

这里没有运行训练营平台的隐藏测试；平台得分以提交后的评测结果为准。

## 重跑合并回归

先按仓库原有说明准备依赖，在仓库根目录执行：

```bash
cmake -S . -B build_debug -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON -DWITH_MEMTRACER=OFF -DWITH_CPPLINGS=OFF
cmake --build build_debug -j8
ASAN_OPTIONS=detect_leaks=0 python3 test/combined_labs_test.py --build-dir build_debug
ASAN_OPTIONS=detect_leaks=0 python3 test/vector_basic_test.py --build-dir build_debug
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build_debug --output-on-failure \
  -R '^(date_type_test|vector_type_test|parser_test|expression_test|buffer_pool_log_test|bplus_tree_log_entry_test)$'
```

合并回归脚本会自动运行上述六组原生 SQL 回归，再创建独立临时数据库完成跨题测试；期望值由 Python 日期和行数据模型计算，保留输出目录供失败排查。

并发日志检查另用独立构建目录：

```bash
cmake -S . -B build_concurrency -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON -DWITH_MEMTRACER=OFF -DWITH_CPPLINGS=OFF -DCONCURRENCY=ON
cmake --build build_concurrency --target mvcc_trx_log_test -j8
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build_concurrency --output-on-failure -R '^mvcc_trx_log_test$'
```

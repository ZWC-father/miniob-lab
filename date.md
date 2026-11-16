# DATE 实验：代码实现与原理

适用分支：`submit-date`。讲解对应的代码提交：[01d509b22c645443a14ab74318e3c78175809ad4](https://github.com/ZWC-father/miniob-lab/commit/01d509b22c645443a14ab74318e3c78175809ad4)。

拿这三条 SQL，按执行顺序讲：

```sql
CREATE TABLE t(d DATE);
INSERT INTO t VALUES ('2020-1-2');
SELECT * FROM t WHERE d > '2019-12-31';
```

表 `t` 可以理解成一组同结构的记录，每条记录只有一个日期字段 `d`。本次实现要让这些语句能够完成：**定义日期字段、写入日期、按日期筛选。**

## 1. 建表：登记日期类型和字段长度

[lex_sql.l](src/observer/sql/parser/lex_sql.l) 新增对 `DATE` 关键字的识别，返回 `DATE_T`；[yacc_sql.y](src/observer/sql/parser/yacc_sql.y) 再把它解释成新增的类型 `AttrType::DATES`。字段不显式指定长度时，沿用原语法规则中的四字节长度。

原有建表流程据此记录字段名、类型、长度。这些描述称为“表的元数据”，后面读写记录都要查它。

类型枚举定义在 [attr_type.h](src/observer/common/type/attr_type.h)。[data_type.cpp](src/observer/common/type/data_type.cpp) 注册新增的 `DateType`，它负责日期的转换、比较和输出；原有的 `Value` 类负责装具体的数据。

## 2. 插入：根据目标字段转换输入

`'2020-1-2'` 刚被解析出来时，类型是字符串 `CHARS`。

原有的 [Table::make_record()](src/observer/storage/table/table.cpp) 发现：输入是字符串，但字段 `d` 要求日期，于是调用类型转换。调用顺序是：

1. 原有 `Value::cast_to()` 根据输入类型选择转换方法。
2. [CharType::cast_to()](src/observer/common/type/char_type.cpp) 中新增的日期分支，调用 `DateType::set_value_from_str()`。
3. `DateType::set_value_from_str()` 检查输入，解析年月日，再生成日期值。

普通字符串字段不会进入这个日期转换过程。

## 3. 日期转换：先校验，再编码

[DateType::set_value_from_str()](src/observer/common/type/date_type.cpp) 是新增实现的核心。

它用 `pos` 遍历输入，把年、月、日分别读进 `parts[3]`。年要求四位，月、日各允许一位或两位，中间必须是 `-`，末尾不能多出字符。

接着检查年份、月份和天数：年份不能为零，月份必须在 1 到 12 之间，每个月的最大天数由 `days_per_month` 给出。二月还要根据闰年调整：

```cpp
int max_day = days_per_month[month - 1];

if (month == 2 && year % 4 == 0
    && (year % 100 != 0 || year % 400 == 0)) {
  max_day++;
}

if (day < 1 || day > max_day) {
  return RC::SCHEMA_FIELD_TYPE_MISMATCH;
}
```

因此 `2000-2-29` 合法，`2017-2-29`、`2017-11-31` 会失败。错误码沿原流程返回，由原框架输出 `FAILURE`，这条记录不会被插入。

检查通过后，执行：

```cpp
val.set_date(year * 10000 + month * 100 + day);
```

`'2020-1-2'` 就变成整数 `20200102`。这样编码后，整数大小与日期先后一致，例如 `20201231 < 20210101`。输入月份和日期是否补零，也不会影响比较结果。

## 4. 存储：将日期整数写入记录

[Value::set_date()](src/observer/common/value.cpp) 是新增函数，将类型设为 `DATES`、数据设为 `20200102`、长度设为四字节。它的声明位于 [value.h](src/observer/common/value.h)。

原有的 `Table::set_value_to_record()` 再把这四字节复制到记录中，后续写入存储仍用原框架。**记录保存日期整数，表的元数据保存“这一列是日期”的信息。**

读取记录时，本次补上的 `Value::set_data()` 日期分支，将这四字节重新装回 `Value`。`Value::set_value()` 和 `Value::get_int()` 也增加了日期处理，供复制和取值使用。

[attr_type.cpp](src/observer/common/type/attr_type.cpp) 中新增的 `"dates"` 名称映射，则使原有元数据保存、恢复流程能够处理日期类型。

## 5. 查询：统一类型后比较

对于：

```sql
SELECT * FROM t WHERE d > '2019-12-31';
```

左边 `d` 是日期，右边仍是字符串。原有的 [LogicalPlanGenerator::create_plan(FilterStmt *, ...)](src/observer/sql/optimizer/logical_plan_generator.cpp) 检查比较双方的类型。

本次在 `CharType::cast_cost()` 中增加字符串到日期的转换代价 `1`，告诉原框架这种转换可用。这个数值用于选择转换方向，不是执行耗时。原框架随后复用第 3 节的转换函数，把右边变成日期。

比较双方因此成为 `20200102` 和 `20191231`。新增的 `DateType::compare()` 执行：

```cpp
return (left_date > right_date) - (left_date < right_date);
```

返回 `-1`、`0`、`1`，分别表示小于、等于、大于。原有的 [ComparisonExpr::compare_value()](src/observer/sql/expr/expression.cpp) 再根据 SQL 运算符判断条件真假；本例条件成立，因此保留这条记录。

日期常量在生成计划时就会校验，所以即使表为空，条件中出现非法日期也会返回失败。

## 6. 索引：复用 B+ 树，修正扫描条件

索引保存字段值与记录位置的对应关系，用于快速查找。原有 B+ 树的 [AttrComparator](src/observer/storage/index/bplus_tree.h) 会调用类型对应的比较方法，因此注册 `DateType` 后，日期索引就能使用上面的整数比较。

本次还修正了 [PhysicalPlanGenerator::create_plan(TableGetLogicalOperator &, ...)](src/observer/sql/optimizer/physical_plan_generator.cpp) 中的一处错误：原框架将 `<>` 也选作等值索引扫描。

例如查询 `d <> '2020-01-01'`，如果先只找出“等于这个日期”的记录，再检查“不等于”，就会把结果全部过滤掉。

修改后，只有 `=` 可以使用这条等值索引扫描路径：

```cpp
if (comparison_expr->comp() != EQUAL_TO) {
  continue;
}
```

其余比较沿用逐行读取、检查条件的全表扫描。

## 7. 输出：将整数还原为日期文本

新增的 `DateType::to_string()` 从 `20200102` 中拆出年月日：

```cpp
// 拆分表达式与实际实现一致，这里用变量展示结果。
int year  = date / 10000;        // 2020
int month = date / 100 % 100;    // 1
int day   = date % 100;          // 2
```

再用 `"%04d-%02d-%02d"` 补零，最终显示 `2020-01-02`。

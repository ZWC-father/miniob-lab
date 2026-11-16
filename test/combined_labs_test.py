"""四题合并回归：python3 test/combined_labs_test.py --build-dir build_debug。

使用独立临时数据库和 Python 日期/行模型校验，不依赖第三方 Python 包。
"""

import argparse
import datetime
import importlib.util
import json
import operator
import os
from pathlib import Path
import random
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, default=ROOT / 'build_debug')
BUILD = parser.parse_args().build_dir.resolve()
WORK = Path(tempfile.mkdtemp(prefix='miniob-combined-'))
os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=0:halt_on_error=1')
CONFIG = WORK / 'observer.ini'
CONFIG.write_text('[LOG]\nLOG_FILE_NAME=observer.log\nLOG_FILE_LEVEL=1\nLOG_CONSOLE_LEVEL=1\n')
checks = 0
print(f'test work directory: {WORK}', flush=True)


class Server:
    def __init__(self, name, args=()):
        self.work = WORK / name
        self.work.mkdir()
        self.args = args
        self.process = self.client = None
        self.log = (self.work / 'server.log').open('ab')

    def start(self):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        self.process = subprocess.Popen([str(BUILD / 'bin/observer'), '-f', str(CONFIG),
                                         '-p', str(port), *self.args], cwd=self.work,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            assert self.process.poll() is None, (self.work, self.process.returncode)
            try:
                self.client = socket.create_connection(('127.0.0.1', port), timeout=1)
                self.client.settimeout(30)
                return
            except ConnectionRefusedError:
                time.sleep(0.05)
        raise AssertionError('server start timeout')

    def stop(self, crash=False):
        if self.client:
            self.client.close()
            self.client = None
        if self.process and self.process.poll() is None:
            self.process.kill() if crash else self.process.terminate()
            status = self.process.wait(timeout=30)
            assert status == (-9 if crash else 0), (self.work, status)
        self.process = None

    def sql(self, command):
        global checks
        self.client.sendall(command.encode() + b'\0')
        data = bytearray()
        while b'\0' not in data:
            chunk = self.client.recv(65536)
            assert chunk, (self.work, command, 'server disconnected')
            data.extend(chunk)
        checks += 1
        return data.split(b'\0')[0].decode().strip()

    def ok(self, command):
        result = self.sql(command)
        assert result == 'SUCCESS', (command[:200], result)

    def fail(self, command):
        result = self.sql(command)
        assert result == 'FAILURE', (command[:200], result)

    def query(self, command, header, rows):
        lines = self.sql(command).splitlines()
        assert lines and lines[0] == header, (command, lines[:5])
        expected = sorted(tuple(str(v) for v in row) for row in rows)
        actual = sorted(tuple(v.strip() for v in line.split('|')) for line in lines[1:])
        assert actual == expected, (command, len(actual), len(expected), actual[:5], expected[:5])

    def absent(self, table):
        db = self.work / 'miniob/db/sys'
        assert not list(db.glob(table + '.*')), list(db.glob(table + '.*'))
        assert not list(db.glob(table + '-*.index'))


def native_regressions():
    spec = importlib.util.spec_from_file_location('native_case_runner', ROOT / 'test/case/miniob_test.py')
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    work = WORK / 'native'
    work.mkdir()
    (work / 'build').symlink_to(BUILD, target_is_directory=True)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    names = ['basic', 'date', 'update', 'primary-date', 'primary-update', 'primary-drop-table']
    options = SimpleNamespace(project_dir=str(ROOT), work_dir=str(work), server_port=port,
                              not_use_unix_socket=True, test_cases=','.join(names), report_only=False)
    suite = native.__init_test_suite(options)
    result = native.EvalResult()
    assert suite.run(result), result.to_json_string()
    message = json.loads(result.to_json_string())['message']
    for name in names:
        assert f'{name} is success' in message, message
    print('PASS native basic/date/update/primary-date/primary-update/primary-drop-table', flush=True)


def fmt(values):
    return '[' + ','.join(f'{x:.5e}' for x in values) + ']'


def mixed(s, mvcc=False):
    s.ok('CREATE TABLE mixed(id int, d date, v vector(8), tail char(8));')
    s.ok('CREATE INDEX mixed_id ON mixed(id);')
    s.ok('CREATE INDEX mixed_d ON mixed(d);')
    s.ok("INSERT INTO mixed VALUES(1,'2000-2-29',to_vector('[1,2,3]'),'keep');")
    s.ok("INSERT INTO mixed VALUES(2,'2020-1-1',to_vector('[4,5]'),'second');")
    s.ok("UPDATE mixed SET d='2024-2-29' WHERE id=1;")
    s.ok("UPDATE mixed SET v=string_to_vector('[9,-2,5,8,7,6,4,3]') WHERE d='2024-2-29';")
    s.ok('UPDATE mixed SET id=-7 WHERE id<>2;')
    s.ok("UPDATE mixed SET tail='x' WHERE v=to_vector('[9,-2,5,8,7,6,4,3]');")
    s.query('SELECT id,d,vector_to_string(v) AS vec,tail FROM mixed;', 'id | d | vec | tail',
            [(-7, '2024-02-29', fmt([9,-2,5,8,7,6,4,3]), 'x'),
             (2, '2020-01-01', fmt([4,5]), 'second')])
    s.query("SELECT id FROM mixed WHERE d='2000-2-29';", 'id', [])
    s.query("SELECT id FROM mixed WHERE '2024-2-29'=d;", 'id', [(-7,)])
    for where in ['', ' WHERE id=999']:
        for value in ["'2023-2-29'", "'1900-2-29'", "'2020-13-1'", '12']:
            s.fail(f'UPDATE mixed SET d={value}{where};')
        for value in ["to_vector('[1,2,3,4,5,6,7,8,9]')", "to_vector('[]')", "'[1,2]'", '7']:
            s.fail(f'UPDATE mixed SET v={value}{where};')
    s.fail('UPDATE mixed SET id=id+1;')
    s.fail("UPDATE mixed SET v=to_vector(tail);")
    s.ok("UPDATE mixed SET v=to_vector('[1]') WHERE id=-7;")
    if mvcc:
        s.ok('BEGIN;')
        s.ok("UPDATE mixed SET v=to_vector('[2,3]') WHERE id=-7;")
        s.ok("UPDATE mixed SET v=to_vector('[8,9]') WHERE id=-7;")
        s.ok("UPDATE mixed SET d='2038-1-19' WHERE id=-7;")
        s.query('SELECT id FROM mixed WHERE id=-7;', 'id', [(-7,)])
        s.ok('ROLLBACK;')
        s.query("SELECT d,from_vector(v) AS v FROM mixed WHERE id=-7;", 'd | v',
                [('2024-02-29', fmt([1]))])
        s.ok('BEGIN;')
        s.ok("UPDATE mixed SET v=to_vector('[6,7]') WHERE id=-7;")
        s.ok("UPDATE mixed SET v=to_vector('[1]') WHERE id=-7;")
        s.ok('COMMIT;')
    s.stop()
    s.start()
    s.query('SELECT id,d,from_vector(v) AS v,tail FROM mixed;', 'id | d | v | tail',
            [(-7, '2024-02-29', fmt([1]), 'x'), (2, '2020-01-01', fmt([4,5]), 'second')])
    if mvcc:
        # 检查点之后的提交也必须恢复事务编号，不能让旧版本重新可见。
        s.ok("UPDATE mixed SET tail='after' WHERE id=2;")
        s.stop(crash=True)
        s.start()
        s.query('SELECT id,tail FROM mixed;', 'id | tail', [(-7, 'x'), (2, 'after')])

    # 长度为 1 的向量行内仍占 16 字节；最大维度不能被当成行内长度。
    for capacity in [1, 16383]:
        s.ok(f'CREATE TABLE boundary(id int, v vector({capacity}), tail int);')
        s.ok("INSERT INTO boundary VALUES(1,to_vector('[1]'),123);")
        literal = '[' + ','.join(['2'] * capacity) + ']'
        s.ok(f"UPDATE boundary SET v=to_vector('{literal}');")
        s.query("SELECT id,distance(v,v,'DOT') AS score,tail FROM boundary;", 'id | score | tail',
                [(1, 4 * capacity, 123)])
        s.ok('DROP TABLE boundary;')
        s.absent('boundary')

    fd_before = len(list(Path(f'/proc/{s.process.pid}/fd').iterdir()))
    for cycle in range(20):
        s.ok('CREATE TABLE reusable(id int, d date, v vector(3));')
        s.ok('CREATE INDEX reused_index ON reusable(d);')
        s.ok("INSERT INTO reusable VALUES(1,'2000-2-29',to_vector('[1,2,3]'));")
        s.ok("UPDATE reusable SET v=to_vector('[3,2,1]');")
        s.ok('DROP TABLE reusable;')
        s.absent('reusable')
        s.fail('SELECT * FROM reusable;')
    assert len(list(Path(f'/proc/{s.process.pid}/fd').iterdir())) <= fd_before + 2
    s.ok('DROP TABLE mixed;')
    s.absent('mixed')
    # 删除成功后立即杀进程，再启动不能回放已删除表的旧索引日志。
    s.stop(crash=True)
    s.start()
    s.fail('SELECT * FROM mixed;')
    s.ok('CREATE TABLE mixed(d date, v vector(1));')
    s.ok("INSERT INTO mixed VALUES('9999-12-31',to_vector('[5]'));")
    s.query('SELECT d,from_vector(v) AS v FROM mixed;', 'd | v', [('9999-12-31', fmt([5]))])
    s.ok('DROP TABLE mixed;')
    s.absent('mixed')


def random_dates_and_updates(s):
    rng = random.Random(20261116)
    s.ok('CREATE TABLE dates(id int, d date, n int);')
    s.ok('CREATE INDEX dates_d ON dates(d);')
    s.ok('CREATE INDEX dates_n ON dates(n);')
    rows = []
    epoch = datetime.date(1900, 1, 1)
    for i in range(400):
        d = epoch + datetime.timedelta(days=rng.randrange(73049))
        rows.append([i, d.isoformat(), i % 13])
        s.ok(f"INSERT INTO dates VALUES({i},'{d.year}-{d.month}-{d.day}',{i % 13});")
    # 用 datetime 独立判断世纪闰年、每月末尾和越界日期。
    for year in [1, 4, 100, 400, 1900, 2000, 2024, 2100, 9999]:
        for month in range(1, 13):
            for day in [28, 29, 30, 31, 32]:
                value = f'{year:04d}-{month}-{day}'
                try:
                    normalized = datetime.date(year, month, day).isoformat()
                except ValueError:
                    s.fail(f"UPDATE dates SET d='{value}' WHERE id=399;")
                else:
                    s.ok(f"UPDATE dates SET d='{value}' WHERE id=399;")
                    rows[399][1] = normalized
                    s.query('SELECT d FROM dates WHERE id=399;', 'd', [(normalized,)])
    operations = {'=': operator.eq, '<>': operator.ne, '<': operator.lt, '>': operator.gt,
                  '<=': operator.le, '>=': operator.ge}
    for step in range(140):
        op = rng.choice(list(operations))
        cutoff = rng.choice(rows)[1]
        pred = lambda row: operations[op](row[1], cutoff)
        n = rng.randrange(-25, 25)
        s.ok(f"UPDATE dates SET n={n} WHERE d{op}'{cutoff}';")
        for row in rows:
            if pred(row):
                row[2] = n
        s.query(f'SELECT id,d,n FROM dates WHERE n={n};', 'id | d | n', [r for r in rows if r[2] == n])
        s.query(f"SELECT id FROM dates WHERE d{op}'{cutoff}';", 'id', [(r[0],) for r in rows if pred(r)])
        s.query(f"SELECT id FROM dates WHERE '{cutoff}'{op}d;", 'id',
                [(r[0],) for r in rows if operations[op](cutoff, r[1])])
        if step % 10 == 0:
            target = rng.choice(rows)[1]
            s.ok(f"UPDATE dates SET d='2000-2-29' WHERE d='{target}';")
            for row in rows:
                if row[1] == target:
                    row[1] = '2000-02-29'
    s.query('SELECT * FROM dates;', 'id | d | n', rows)
    s.stop()
    s.start()
    s.query('SELECT * FROM dates;', 'id | d | n', rows)
    s.query("SELECT id FROM dates WHERE d='2000-2-29';", 'id', [(r[0],) for r in rows if r[1] == '2000-02-29'])
    s.ok('DROP TABLE dates;')
    s.absent('dates')


native_regressions()
for name, args in [('default', []), ('disk', ['-d']), ('mvcc', ['-d', '-t', 'mvcc'])]:
    server = Server(name, args)
    try:
        server.start()
        mixed(server, mvcc=name == 'mvcc')
        if name == 'disk':
            random_dates_and_updates(server)
        print(f'PASS {name}: mixed DATE/VECTOR/UPDATE, indexes, DROP cleanup, restart', flush=True)
    finally:
        server.stop()
        server.log.close()
    assert 'AddressSanitizer' not in (server.work / 'server.log').read_text()
print(f'PASS {checks} combined SQL assertions plus native regressions', flush=True)

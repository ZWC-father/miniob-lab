"""Run with python3 test/vector_basic_test.py --build-dir build_debug.

Uses only the Python standard library; starts an isolated server and also runs
MiniOB's original basic regression against the supplied build.
"""

import argparse
import math
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

REPO = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description="vector-basic SQL, persistence and basic regression")
parser.add_argument('--build-dir', type=Path, default=REPO / 'build_debug')
BUILD = parser.parse_args().build_dir.resolve()
os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
work = Path(tempfile.mkdtemp(prefix='miniob-vector-check-'))
print(f'test work directory: {work}', flush=True)
log = (work / 'server.log').open('wb')
probe = socket.socket()
probe.bind(('127.0.0.1', 0))
port = probe.getsockname()[1]
probe.close()
process = None
client = None
checks = 0


def start():
    global process, client
    process = subprocess.Popen([str(BUILD / 'bin/observer'), '-f', str(REPO / 'etc/observer.ini'), '-p', str(port)],
                               cwd=work, stdout=log, stderr=log)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f'server exited: {process.returncode}')
        try:
            client = socket.create_connection(('127.0.0.1', port), timeout=2)
            client.settimeout(10)
            return
        except ConnectionRefusedError:
            time.sleep(0.05)
    raise AssertionError('server did not start')


def stop():
    global process, client
    if client:
        client.close()
        client = None
    if process:
        process.terminate()
        code = process.wait(timeout=15)
        process = None
        assert code == 0, f'server exited with {code}'


def sql(command):
    global checks
    checks += 1
    client.sendall(command.encode() + b'\0')
    response = bytearray()
    while b'\0' not in response:
        data = client.recv(65536)
        assert data, f'server closed on {command}'
        response.extend(data)
    return response.split(b'\0')[0].decode().strip()


def ok(command):
    result = sql(command)
    assert result == 'SUCCESS', (command, result)


def fail(command):
    result = sql(command)
    assert result == 'FAILURE', (command, result)


def query(command, header, expected):
    result = sql(command)
    lines = [tuple(part.strip() for part in line.split('|')) for line in result.splitlines()]
    assert lines and lines[0] == tuple(header), (command, result[:1000])
    actual = sorted(lines[1:])
    expected = sorted(tuple(str(part) for part in row) for row in expected)
    assert actual == expected, (command, 'actual', actual[:15], 'expected', expected[:15], len(actual), len(expected))



def scalar(command):
    lines = sql(command).splitlines()
    assert len(lines) == 2, (command[:200], lines[:5])
    return lines[1]


def numeric(command, expected):
    actual = float(scalar(command))
    assert math.isclose(actual, expected, rel_tol=1e-5, abs_tol=0.0051), (command, actual, expected)


def text_vector(values):
    return '[' + ','.join(str(value) for value in values) + ']'


def formatted(values):
    return '[' + ','.join(f'{value:.5e}' for value in values) + ']'


try:
    start()
    # Reproduce the judge failure exactly, including its SELECT output alias.
    ok('CREATE TABLE TEST (ID INT, C1 VECTOR(3));')
    judge_vectors = [
        '[3.84,1.6,5.6]', '[-2.0,2.42,9.53]', '[-0.89,1.83,-6.26]',
        '[-1.64,-7.0,-3.16]', '[-7.01,-2.19,-3.66]', '[-2.81,-9.56,2.46]',
    ]
    judge_expected = [
        (1, '[3.84000e+00,1.60000e+00,5.60000e+00]'),
        (2, '[-2.00000e+00,2.42000e+00,9.53000e+00]'),
        (3, '[-8.90000e-01,1.83000e+00,-6.26000e+00]'),
        (4, '[-1.64000e+00,-7.00000e+00,-3.16000e+00]'),
        (5, '[-7.01000e+00,-2.19000e+00,-3.66000e+00]'),
        (6, '[-2.81000e+00,-9.56000e+00,2.46000e+00]'),
    ]
    for i, v in enumerate(judge_vectors, 1):
        ok(f"INSERT INTO TEST VALUES({i}, STRING_TO_VECTOR('{v}'));")
    query('SELECT ID, VECTOR_TO_STRING(C1) AS VEC_STR FROM TEST;',
          ['ID', 'VEC_STR'], judge_expected)
    query('SELECT ID AS ROW_ID, FROM_VECTOR(C1) AS VEC_STR FROM TEST;',
          ['ROW_ID', 'VEC_STR'], judge_expected)
    query('SELECT TEST.ID AS ROW_ID, VECTOR_TO_STRING(TEST.C1) as Vec_Str FROM TEST;',
          ['ROW_ID', 'Vec_Str'], judge_expected)
    query('SELECT TEST.ID, VECTOR_TO_STRING(C1) FROM TEST;',
          ['ID', 'VECTOR_TO_STRING(C1)'], judge_expected)
    query('SELECT ID AS C1, VECTOR_TO_STRING(C1) AS ID FROM TEST WHERE ID=1;',
          ['C1', 'ID'], judge_expected[:1])
    query("SELECT VECTOR_TO_STRING(STRING_TO_VECTOR('[1,2,3]')) AS VEC_STR, 1 AS ID;",
          ['VEC_STR', 'ID'], [(formatted([1,2,3]), 1)])
    query('SELECT * FROM TEST WHERE ID=1;', ['ID', 'C1'], judge_expected[:1])
    for command in [
        'SELECT VECTOR_TO_STRING(C1) AS FROM TEST;',
        'SELECT VECTOR_TO_STRING(C1 AS V) FROM TEST;',
        'SELECT ID FROM TEST WHERE ID AS X=1;',
        'SELECT * AS X FROM TEST;',
    ]:
        fail(command)
    print('judge reproducer: exact six rows, output aliases and syntax boundaries: passed', flush=True)

    ok('create table items (id int, v vector(3), w vector(3), label char(12));')
    ok("insert into items values (1, string_to_vector('[1,2,3]'), to_vector('[4,6,3]'), 'first');")
    ok("insert into items values (2, to_vector('[0,1,0]'), to_vector('[1,0,0]'), 'second');")
    # VECTOR(N) is a maximum capacity in the 2025 specification.
    ok("insert into items values (3, to_vector('[1,2]'), to_vector('[1,2]'), 'short');")
    query('select id, vector_to_string(v), from_vector(w), label from items;',
          ['id', 'vector_to_string(v)', 'from_vector(w)', 'label'],
          [(1, formatted([1,2,3]), formatted([4,6,3]), 'first'),
           (2, formatted([0,1,0]), formatted([1,0,0]), 'second'),
           (3, formatted([1,2]), formatted([1,2]), 'short')])
    query("select id from items where v=to_vector('[1,2,3]');", ['id'], [(1,)])
    query("select id from items where to_vector('[1,2,3]')<>v;", ['id'], [(2,), (3,)])
    numeric("select distance(v,w,'EUCLIDEAN') from items where id=1;", 5)
    numeric("select vector_distance(v,w,'DOT') from items where id=1;", 25)
    numeric("select distance(v,w,'cOsInE') from items where id=2;", 1)
    for name, expected in [('l2_distance',5), ('inner_product',25), ('cosine_distance',1-25/math.sqrt(14*61))]:
        numeric(f'select {name}(v,w) from items where id=1;', expected)
        numeric(f'select {name}(v,w) AS DIST from items where id=1;', expected)
    for name in ['DISTANCE', 'VECTOR_DISTANCE']:
        query(f"SELECT id AS ID, {name}(v,w,'EUCLIDEAN') AS DIST FROM items WHERE id=1;",
              ['ID', 'DIST'], [(1, 5)])
    query("SELECT distance(v,w,'DOT')+1 AS SCORE FROM items WHERE id=1;", ['SCORE'], [(26,)])
    query("SELECT vector_to_string(to_vector('[1,2]')) AS VEC_STR FROM items WHERE id=1;",
          ['VEC_STR'], [(formatted([1,2]),)])
    query("select id from items where distance(v,w,'EUCLIDEAN')=5;", ['id'], [(1,)])
    assert scalar('select vector_to_string(string_to_vector(" [1.05, -17.8, 3.2e1] "));') == '[1.05000e+00,-1.78000e+01,3.20000e+01]'
    print('conversion aliases, nested expressions, equality and all distances: passed', flush=True)

    for command in [
        'create table bad0(v vector(0));', 'create table bad1(v vector(16384));',
        'create table bad2(v vector());', 'create table bad3(v vector(-1));',
        'create table bad4(v vector(4294967297));',
        "insert into items values(4,to_vector('[1,2,3,4]'),to_vector('[1]'),'bad');",
        "insert into items values(4,'[1,2,3]',to_vector('[1]'),'bad');",
        "select id from items where v='[1,2,3]';", 'select id from items where v>v;',
        'select id from items where v<1;', 'select id from items where 1=v;',
        'create index invalid_vector_key on items(v);',
        'select string_to_vector(12);', "select vector_to_string('[1,2]');",
        "select string_to_vector('[1]', '[2]');",
        "select distance(to_vector('[1]'),to_vector('[1]'),'invalid');",
        "select distance(to_vector('[1]'),to_vector('[1]'),'l2_distance');",
        "select distance(to_vector('[1]'),to_vector('[1,2]'),'DOT');",
        "select distance(to_vector('[0]'),to_vector('[1]'),'COSINE');",
        # The third row differs in dimension: the entire query must fail cleanly.
        "select distance(v,to_vector('[1,2,3]'),'DOT') from items;",
        "select id from items where distance(v,to_vector('[1,2,3]'),'DOT')>0;",
    ]:
        fail(command)
    for malformed in ['', '[]', '[ ]', '1,2', '[1,]', '[1,,2]', '[1 2]', '[nan]', '[Infinity]', '[0x1p2]', '[1e]', '[1e999]', '[1]x']:
        fail(f"select string_to_vector('{malformed}');")
    ok('create table empty_vectors(v vector(3));')
    fail("select distance(v,to_vector('[1,2,3]'),'bad') from empty_vectors;")
    fail("select distance(v,to_vector('[1,]'),'DOT') from empty_vectors;")
    print('invalid syntax, types, dimensions and late-row failures: passed', flush=True)

    ok('create table defaults(id int, v vector, tail int);')
    default = [1] * 2048
    ok(f"insert into defaults values(1,to_vector('{text_vector(default)}'),99);")
    numeric("select distance(v,v,'DOT') from defaults;", 2048)
    fail(f"insert into defaults values(2,to_vector('{text_vector(default+[1])}'),100);")
    ok('create table maximums(id int, v vector(16383), tail int);')
    maximum = [1] * 16383
    ok(f"insert into maximums values(1,to_vector('{text_vector(maximum)}'),77);")
    numeric("select distance(v,v,'DOT') from maximums;", 16383)
    assert scalar('select from_vector(v) from maximums;') == formatted(maximum)
    fail(f"insert into maximums values(2,to_vector('{text_vector(maximum+[1])}'),78);")
    print('default 2048 and maximum 16383 entries, oversized packets and output: passed', flush=True)

    ok('create table random_vectors(id int, v vector(16), w vector(16));')
    rng = random.Random(2025)
    rows = []
    for i in range(120):
        a = [rng.randint(-100,100) for _ in range(16)]
        b = [rng.randint(-100,100) for _ in range(16)]
        ok(f"insert into random_vectors values({i},to_vector('{text_vector(a)}'),to_vector('{text_vector(b)}'));")
        rows.append((i,a,b))
    ok('create index normal_id_index on random_vectors(id);')
    for i,a,b in rows:
        dot = sum(x*y for x,y in zip(a,b))
        numeric(f"select distance(v,w,'DOT') from random_vectors where id={i};", dot)
        numeric(f"select distance(v,w,'EUCLIDEAN') from random_vectors where id={i};", math.sqrt(sum((x-y)**2 for x,y in zip(a,b))))
        numeric(f"select distance(v,w,'COSINE') from random_vectors where id={i};", 1-dot/math.sqrt(sum(x*x for x in a)*sum(y*y for y in b)))
    print('120 random vector pairs checked against independent Python calculations: passed', flush=True)
    stop()
    start()
    query('SELECT ID, VECTOR_TO_STRING(C1) AS VEC_STR FROM TEST;',
          ['ID', 'VEC_STR'], judge_expected)
    numeric("select distance(v,v,'DOT') from defaults;", 2048)
    numeric("select distance(v,v,'DOT') from maximums;", 16383)
    query('select id,tail from maximums;', ['id','tail'], [(1,77)])
    query('select id from items where v=w;', ['id'], [(3,)])
    for i,a,b in rows[::10]:
        numeric(f"select distance(v,w,'DOT') from random_vectors where id={i};", sum(x*y for x,y in zip(a,b)))
    ok("delete from items where v=to_vector('[1,2]');")
    query('select id from items;', ['id'], [(1,), (2,)])
    stop()
    print('restart: vector metadata, LOB data, normal index and equality deletion: passed', flush=True)

    spec = importlib.util.spec_from_file_location('native_case_runner', REPO / 'test/case/miniob_test.py')
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    native_work = work / 'basic_regression'
    native_work.mkdir()
    (native_work / 'build').symlink_to(BUILD, target_is_directory=True)
    opts = SimpleNamespace(project_dir=str(REPO), work_dir=str(native_work), server_port=port,
                           not_use_unix_socket=True, test_cases='basic', report_only=False)
    suite = native.__init_test_suite(opts)
    result = native.EvalResult()
    assert suite.run(result), result.to_json_string()
    report = json.loads(result.to_json_string())
    assert 'basic is success' in report['message'], report
    print(f'PASS: {checks} SQL requests plus the native basic regression', flush=True)
except BaseException:
    if client:
        client.close()
    if process and process.poll() is None:
        process.terminate()
        process.wait(timeout=15)
    log.flush()
    print((work / 'server.log').read_text(errors='replace')[-5000:], flush=True)
    raise
finally:
    log.close()

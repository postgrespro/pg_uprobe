from pathlib import Path
import re
from decimal import Decimal
from testgres import PostgresNode
from testgres import NodeConnection
from time import sleep
from utils import node_read_file_one_line, test_wrapper, node_get_file_size, load_scripts, extract_numbers_as_strings_from_time_point
import json


def start_session_trace(conn: NodeConnection):
    conn.execute("select start_session_trace()")


def start_session_trace_pid(conn: NodeConnection, pid: int):
    conn.execute(f"select start_session_trace({pid})")


def stop_session_trace_pid(conn: NodeConnection, pid: int):
    conn.execute(f"select stop_session_trace({pid})")


def stop_session_trace(conn: NodeConnection):
    conn.execute("select stop_session_trace()")


def validate_explain_field(json):
    assert type(json) is dict

    assert 'Query Text' in json

    assert 'Plan' in json


def validate_trace_data_field(json):
    assert type(json) is dict

    assert 'maxTime' in json
    assert type(json['maxTime']) is int

    assert 'totalCalls' in json
    assert type(json['totalCalls']) is int

    assert 'totalTimeSum' in json
    assert type(json['totalTimeSum']) is int

    if json['totalCalls'] > 0:
        assert json['maxTime'] > 0
        assert json['totalTimeSum'] > 0
        assert json['maxTime'] <= json['totalTimeSum']



def validate_explain_with_node_stat_field(json):
    assert type(json) is dict

    assert 'traceData' in json

    validate_trace_data_field(json['traceData'])

    if 'Plans' in json:
        assert type(json['Plans']) is list
        for subplan in json['Plans']:
            validate_explain_with_node_stat_field(subplan)



def validate_nanosec_time_field(json, grater_than_zero = True):
    assert type(json) is str

    numbers  = re.findall(r'\d+', json)

    assert len(numbers) == 1

    if grater_than_zero:
        assert int(numbers[0]) > 0, "All times should be grater than zero"


def validate_wait_event_stat_object(json):
    assert type(json) is dict

    assert 'name' in json

    assert type(json['name']) is str

    assert 'count' in json

    assert type(json['count']) is int

    assert json['count'] > 0

    assert 'timeSum' in json

    validate_nanosec_time_field(json['timeSum'])

    assert 'maxTime' in json

    validate_nanosec_time_field(json['maxTime'])


def validate_wait_event_stat_field(json):
    assert type(json) is list

    for stat in json:
        validate_wait_event_stat_object(stat)


def validate_buffer_tag_object(json):
    assert type(json) is dict

    assert "spcOid" in json
    assert type(json['spcOid']) is int

    assert "spcName" in json
    assert type(json['spcName']) is str

    assert "dbOid" in json
    assert (type(json["dbOid"])) is int

    assert "dbName" in json
    assert type(json['dbName']) is str

    assert "relNumber" in json
    assert (type(json["relNumber"])) is int

    assert "relName" in json
    assert type(json['relName']) is str

    assert "relKind" in json
    assert (type(json["relKind"])) is str

    assert "forkName" in json
    assert (type(json["forkName"])) is str

    assert "blockNumber" in json
    assert (type(json["blockNumber"])) is int


def validate_lwlock_stat_calls_object(json):
    assert type(json) is dict

    assert "totalCalls" in json
    assert type(json['totalCalls']) is int

    assert "sleepCount" in json
    assert type(json['sleepCount']) is int

    assert "sleepTimeSum" in json
    validate_nanosec_time_field(json["sleepTimeSum"], json['sleepCount'] > 0)

    assert "maxSleepTime" in json
    validate_nanosec_time_field(json["maxSleepTime"], json['sleepCount'] > 0)



def validate_LWLock_stat_object(json):
    assert type(json) is dict

    assert "bufferTag" in json
    validate_buffer_tag_object(json["bufferTag"])

    assert "exclusive" in json
    validate_lwlock_stat_calls_object(json["exclusive"])

    assert "shared" in json
    validate_lwlock_stat_calls_object(json["shared"])



def validate_LWLock_stat_field(json):
    assert type(json) is list

    for obj in json:
        validate_LWLock_stat_object(obj)


def validate_execution_event_object(json):
    assert type(json) is dict

    if "executionEvents" in json:
        validate_one_query_trace(json)
        return

    assert 'node' in json

    assert type(json['node']) is str

    assert 'executeTime' in json

    validate_nanosec_time_field(json['executeTime'])

    if 'explain' in json:
        assert type(json['explain']) is dict

    if 'LWLockStat' in json:
        validate_LWLock_stat_field(json['LWLockStat'])




def validate_execution_events_filed(json):
    assert type(json) is list

    for obj in json:
        validate_execution_event_object(obj)


def validate_execution_start_field(json):
    assert type(json) is str
    #json should look like this "2025:06:18T12:17:31.006"

    numbers = extract_numbers_as_strings_from_time_point(json)

    assert len(numbers) == 7

    assert len(numbers[0]) == 4, "len of year should be 4"
    assert len(numbers[-1]) == 3, "expect only ms part of nanoseconds"

    for i in range(1, (len(numbers) - 1)):
        assert len(numbers[i]) == 2

def validate_one_query_trace(json):
    assert type(json) is dict

    # for cases where last item in json array is empty
    if len(json) == 0:
        return

    if 'parsingTime' in json:
        validate_nanosec_time_field(json['parsingTime'])

    if 'planningTime' in json:
        validate_nanosec_time_field(json['planningTime'])

    assert 'executionStart' in json

    validate_execution_start_field(json['executionStart'])

    assert 'explain' in json

    validate_explain_field(json['explain'])

    assert 'executionEvents' in json

    validate_execution_events_filed(json['executionEvents'])

    assert 'executionTime' in json

    validate_nanosec_time_field(json['executionTime'])

    if 'executorNodeStatInPlan' in json:

        validate_explain_with_node_stat_field(json['executorNodeStatInPlan'])

    if 'waitEventStat' in json:
        validate_wait_event_stat_field(json['waitEventStat'])

    if 'LWLockPlanning' in json:
        validate_LWLock_stat_field(json['LWLockPlanning'])

    if 'LWLockParsing' in json:
        validate_LWLock_stat_field(json['LWLockParsing'])

    if 'locksInsidePortalRun' in json:
        validate_LWLock_stat_field(json['locksInsidePortalRun'])

    if 'locksOutsidePortalRun' in json:
        validate_LWLock_stat_field(json['locksOutsidePortalRun'])


def validate_session_trace_result_simple(json, pid):
    assert type(json) is dict
    assert 'pid' in json
    assert type(json['pid']) is int
    assert json['pid'] == pid
    assert 'queries' in json
    assert type(json['queries']) is list
    assert len(json['queries']) == 2, "in this trace file should be only 2 queries"

    validate_one_query_trace(json['queries'][0])


def validate_each_session_trace_result(json, pid):
    assert type(json) is dict
    assert 'pid' in json
    assert type(json['pid']) is int
    assert json['pid'] == pid
    assert 'queries' in json
    assert type(json['queries']) is list

    for obj in json['queries']:
        validate_one_query_trace(obj)


def trace_current_session_trace(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")

        validate_session_trace_result_simple(json.loads(result), conn.pid)


def trace_current_session_trace_non_sleep_buffer_locks(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        conn.execute("set pg_uprobe.write_only_sleep_lwlocks_stat to false")

        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")

        validate_session_trace_result_simple(json.loads(result), conn.pid)


def trace_current_session_trace_non_sleep_buffer_locks_for_each_node(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        conn.execute("set pg_uprobe.write_only_sleep_lwlocks_stat to false")

        conn.execute("set pg_uprobe.trace_lwlocks_for_each_node to true")

        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")


        validate_session_trace_result_simple(json.loads(result), conn.pid)


def trace_current_session_change_data_dir(node: PostgresNode):
    Path("/tmp/test_session_trace_dir").mkdir(parents=True, exist_ok=True)

    with node.connect("postgres", autocommit=True) as conn:

        conn.execute("set pg_uprobe.data_dir to '/tmp/test_session_trace_dir'")

        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = open(f"/tmp/test_session_trace_dir/trace_file.txt_{conn.pid}").read(-1)

        validate_session_trace_result_simple(json.loads(result), conn.pid)


def trace_current_session_change_trace_file_name(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:

        conn.execute("set pg_uprobe.trace_file_name to 'trace'")

        conn.execute("set pg_uprobe.write_only_sleep_lwlocks_stat to false")

        conn.execute("set pg_uprobe.trace_lwlocks_for_each_node to true")

        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_{conn.pid}")

        validate_session_trace_result_simple(json.loads(result), conn.pid)


def trace_current_session_check_file_limit(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:

        conn.execute("set pg_uprobe.write_only_sleep_lwlocks_stat to false")

        conn.execute("set pg_uprobe.trace_lwlocks_for_each_node to true")

        start_session_trace(conn)

        for i in range(100000):
            conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_get_file_size(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")

        assert (result/1024/1024) > 16 and (result/1024/1024) < 17, "result file can be a little bigger than 16mb, but can't be too big"


def trace_current_session_change_check_file_limit(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:

        conn.execute("set pg_uprobe.trace_file_limit to 32")

        conn.execute("set pg_uprobe.write_only_sleep_lwlocks_stat to false")

        conn.execute("set pg_uprobe.trace_lwlocks_for_each_node to true")

        start_session_trace(conn)

        for i in range(100000):
            conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_get_file_size(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")

        assert (result/1024/1024) > 32 and (result/1024/1024) < 33, "result file can be a little bigger than 32mb, but can't be too big"


def trace_current_session_write_mod(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        conn.execute("set pg_uprobe.trace_write_mode TO text")

        start_session_trace(conn)

        conn.execute("select * from pgbench_accounts LIMIT 5")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")


        has_error = False

        try:
            validate_session_trace_result_simple(json.loads(result), conn.pid)
        except:
            has_error = True

        assert has_error, "result should't be json"


def trace_current_session_large(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        for sql in load_scripts():
            conn.execute(sql)


        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")

        validate_each_session_trace_result(json.loads(result), conn.pid)


def trace_session_pid(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        pid:int
        with node.connect("postgres", autocommit=True) as node_trace:
            pid = node_trace.pid
            start_session_trace_pid(conn, pid)

            for sql in load_scripts():
                node_trace.execute(sql)


            stop_session_trace_pid(conn, pid)


        sleep(1)
        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{pid}")
        validate_each_session_trace_result(json.loads(result), pid)


def trace_session_plpgsql_functions(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        assert conn.execute("select calculate_order_total(2)") == [(Decimal('80000.00'),)]
        conn.execute("select update_order_status(2, 'SHIPPED')")
        conn.execute("select add_product_to_order(1, 2, 100)")
        assert conn.execute("select * from get_user_orders(1)") == [(1, Decimal('4050000.00'), 'NEW', 2,)]
        conn.execute("select create_user('aboba', 'boba', 'aboba_boba@example.ru', '+79132281337')")
        assert conn.execute("select * from get_low_stock_products(10000)") == [(2,"Samsung Galaxy S10",50,40000.00),(1,"iPhone X",100,50000.00), (3,"Xiaomi Redmi Note 8 Pro",200,25000.00)]
        assert conn.execute("SELECT process_complete_order(1, ARRAY[2,3], ARRAY[10, 5])") == [(4,)]

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)


def trace_session_plpgsql_functions_exceptions(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        try:
            conn.execute("SELECT process_complete_order(1, ARRAY[5,8], ARRAY[10, 5])")
        except:
            pass

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)


def trace_session_correct_executor_finish(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        conn.execute("create table mlparted (a int, b int)")

        conn.execute("with ins (a, b, c) as \
                        (insert into mlparted (b, a) select s.a, 1 from generate_series(2, 39) s(a) returning tableoid::regclass, *) \
                        select a, b, min(c), max(c) from ins group by a, b order by 1;")

        conn.execute("drop table mlparted")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)

def trace_session_fetch(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        conn.execute("CREATE TABLE INT8_TBL(q1 int8, q2 int8)")

        conn.execute("INSERT INTO INT8_TBL VALUES \
                        ('  123   ','  456'), \
                        ('123   ','4567890123456789'), \
                        ('4567890123456789','123'), \
                        (+4567890123456789,'4567890123456789'), \
                        ('+4567890123456789','-4567890123456789')")
        conn.execute("begin")
        conn.execute("create function nochange(int) returns int \
                      as 'select $1 limit 1' language sql stable")
        conn.execute("declare c cursor for select * from int8_tbl limit nochange(3)")
        conn.execute("fetch all from c")
        conn.execute("move backward all in c")
        conn.execute("fetch all from c")
        conn.execute("rollback")
        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)

def trace_session_correct_with_jit(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        conn.execute("set jit to on")
        conn.execute("set jit_above_cost to 0.0")
        conn.execute("set jit_inline_above_cost to 0.0")
        conn.execute("set jit_optimize_above_cost to 0.0")
        start_session_trace(conn)

        conn.execute("create table mlparted (a int, b int)")
        conn.execute("with ins (a, b, c) as \
                        (insert into mlparted (b, a) select s.a, 1 from generate_series(2, 39) s(a) returning tableoid::regclass, *) \
                        select a, b, min(c), max(c) from ins group by a, b order by 1;")

        conn.execute("drop table mlparted")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)

def trace_session_correct_fetch_zero_desc(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        start_session_trace(conn)

        conn.execute("CREATE FUNCTION create_temp_tab() RETURNS text \
                        LANGUAGE plpgsql AS $$ \
                        BEGIN \
                        CREATE TEMP TABLE new_table (f1 float); \
                        INSERT INTO new_table SELECT invert(0.0); \
                        RETURN 'foo'; \
                    END $$;")
        conn.execute("BEGIN")
        conn.execute("DECLARE ctt CURSOR FOR SELECT create_temp_tab()")
        conn.execute("SAVEPOINT s1")
        try:
            conn.execute("FETCH ctt")
        except:
            pass
        conn.execute("ROLLBACK TO s1")
        try:
            conn.execute("FETCH ctt")
        except:
            pass
        conn.execute("ROLLBACK")

        stop_session_trace(conn)

        result = node_read_file_one_line(node, f"/pg_uprobe/trace_file.txt_{conn.pid}")
        validate_each_session_trace_result(json.loads(result), conn.pid)

def run_tests(node: PostgresNode):
    test_wrapper(node, trace_current_session_trace)
    test_wrapper(node, trace_current_session_trace_non_sleep_buffer_locks)
    test_wrapper(node, trace_current_session_trace_non_sleep_buffer_locks_for_each_node)

    test_wrapper(node, trace_current_session_change_data_dir)
    test_wrapper(node, trace_current_session_change_trace_file_name)
    test_wrapper(node, trace_current_session_check_file_limit)
    test_wrapper(node, trace_current_session_change_check_file_limit)
    test_wrapper(node, trace_current_session_write_mod)

    test_wrapper(node, trace_current_session_large)
    test_wrapper(node, trace_session_pid)
    test_wrapper(node, trace_session_plpgsql_functions)
    test_wrapper(node, trace_session_plpgsql_functions_exceptions)
    test_wrapper(node, trace_session_correct_executor_finish)
    test_wrapper(node, trace_session_fetch)
    test_wrapper(node, trace_session_correct_with_jit)
    test_wrapper(node, trace_session_correct_fetch_zero_desc)
from pathlib import Path
import re
import random
import os
from decimal import Decimal
from testgres import PostgresNode
from testgres import NodeConnection
from time import sleep

from utils import node_read_file, test_wrapper

test_multy_functions = ["PortalStart", "PortalRun", "GetCachedPlan", "index_getnext_tid",
                        "fopen", "fclose", "tuplesort_performsort", "SerializeSnapshot",
                        "LockBuffer", "ReleaseCachedPlan", "PushActiveSnapshot",
                        "pg_parse_query", "CreatePortal", "ExecInitNode",
                        "ExecAssignExprContext", "index_beginscan", "index_getnext_slot",
                        "LWLockAcquire", "LWLockRelease", "index_endscan"]


def node_has_file(node: PostgresNode, path: str) -> bool:
    return os.path.isfile(node.data_dir + path)


def set_uprobe(conn: NodeConnection, func: str, type: str, is_shared: bool):
    conn.execute(f"select set_uprobe('{func}', '{type}', {is_shared})")


def dump_uprobe_stat(conn: NodeConnection, func: str, should_clean: bool):
    conn.execute(f"select dump_uprobe_stat('{func}', {should_clean})")



def check_hist_on_percent_values(tuples: list[tuple]):
    for t in tuples:
        assert len(t) == 3, "In each tupple should be 3 lines"


    is_non_zero = False
    percent_summ = Decimal(0.000)
    for t in tuples:
        if (t[2] != Decimal(0.000)):
            is_non_zero = True
            percent_summ += t[2]

    assert is_non_zero, "At least one value in third column should be grater than zero for this query"

    assert abs(percent_summ - Decimal(100.0)) < 1.0, "Summ of all percents should be close to close to 100"



def check_TIME_uprobe_file(file_lines: list[str], should_have_calls: bool):
    assert len(file_lines) == 1, "In this file should be only one line"

    result_numbers = re.findall(r'\d+', file_lines[0])

    if should_have_calls:
        assert int(result_numbers[0]) > 0, "There must be calls of function"
        assert int(result_numbers[1]) > 0, "Time summ of calls can't be 0 or negative"
    else:
        assert int(result_numbers[0]) >= 0, "Number of calls can't be negative"
        assert int(result_numbers[1]) >= 0, "Time summ of calls can't be  negative"


def TIME_uprobe_get_stat(file_lines: list[str]) -> tuple:
    result_numbers = re.findall(r'\d+', file_lines[0])
    return (int(result_numbers[0]), int(result_numbers[1]))


def check_HIST_uprobe_file(file_lines: list[str]):
    assert len(file_lines) >= 1, "in this file should be at least 1 line"

    for line in file_lines[1:]:
        assert line.count(',') == 1, "this is csv file with 2 colums, so only one comma"
        line_numbers = line.split(',')
        assert Decimal(line_numbers[0]) >= 0, "call time can't be negative"
        assert int (line_numbers[1]) > 0, "number of calls must be grater than zero"


def check_MEM_uprobe_file(file_lines: list[str]):
    assert len(file_lines) >= 1, "in this file should be more than 1 line"

    for line in file_lines[1:]:
        assert line.count(',') == 1, "this is csv file with 2 colums, so only one comma"
        line_numbers = line.split(',')
        assert int (line_numbers[1]) > 0, "number of calls must be grater than zero"


def test_TIME_urpobe_local(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'TIME', False)
        conn.execute("select * from pg_class")
        result = conn.execute("select stat_time_uprobe('PortalStart')")[0][0]
        result_numbers = re.findall(r'\d+', result)
        assert int(result_numbers[0]) == 2, "There were two queries, so PortalStart should be called 2 times"
        assert int(result_numbers[1]) > 0, "Time summ of two call can't be 0 or negative"


def test_HIST_uprobe_local(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'HIST', False)
        conn.execute("select * from pg_class")

        #fist stat_hist_uprobe_test
        result = conn.execute("select * from stat_hist_uprobe('PortalStart')")

        assert len(result) == 4, "In first call of stat_hist_urpobe the result should be 4 tuples"

        check_hist_on_percent_values(result)

        #second stat_hist_urpobe test
        result = conn.execute("select * from stat_hist_uprobe('PortalStart', 0.0, 100.0, 10.0)")

        assert len(result) == 12, "In this of stat_hist_urpobe the result should be 12 tuples"

        check_hist_on_percent_values(result)


def test_TIME_uprobe_shared(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'TIME', True)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        dump_uprobe_stat(conn, 'PortalStart', True)
        result = node_read_file(node, "/pg_uprobe/TIME_PortalStart.txt")

        check_TIME_uprobe_file(result, True)
        conn.execute("select delete_uprobe(func, false) from list_uprobes()")

def test_HIST_uprobe_shared(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'HIST', True)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        dump_uprobe_stat(conn, 'PortalStart', True)
        result = node_read_file(node, "/pg_uprobe/HIST_PortalStart.txt")

        assert len(result) > 1, "in this file should be more than 1 line"

        check_HIST_uprobe_file(result)

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")

def test_MEM_uprobe_shared(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'MEM', True)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        dump_uprobe_stat(conn, 'PortalStart', True)
        result = node_read_file(node, "/pg_uprobe/MEM_PortalStart.txt")

        check_MEM_uprobe_file(result)

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


def test_multy_TIME_local_uprobes(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "TIME", False)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:
            result = conn.execute(f"select stat_time_uprobe('{func}')")[0][0]
            result_numbers = re.findall(r'\d+', result)
            assert len(result_numbers) == 2

        for func in functions_reorder:
            conn.execute(f"select delete_uprobe('{func}', false)")

        assert len(conn.execute("select * from list_uprobes()")) == 0

def test_multi_HIST_local_uprobes(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "HIST", False)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:
            result = conn.execute(f"select * from stat_hist_uprobe('{func}')")

            assert len(result) > 3 or len(result) == 0, "In result histgram should be empty or have at least 4 lines"

            if len(result) != 0:
                check_hist_on_percent_values(result)

        for func in functions_reorder:
            result = conn.execute(f"select * from stat_hist_uprobe('{func}', 0.0, 100.0, 10.0)")

            assert len(result) == 12 or len(result) == 0, "In result histgram should be empty or have at least 4 lines"

            if len(result) != 0:
                check_hist_on_percent_values(result)


        for func in functions_reorder:
            conn.execute(f"select delete_uprobe('{func}', false)")

        assert len(conn.execute("select * from list_uprobes()")) == 0


def test_multi_TIME_shared_uprobes(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "TIME", True)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:
            conn.execute(f"select dump_uprobe_stat('{func}', true)")
            sleep(0.2) #just in case
            file_lines = node_read_file(node, f"/pg_uprobe/TIME_{func}.txt")

            check_TIME_uprobe_file(file_lines, False)

        for func in functions_reorder:
            conn.execute(f"select delete_uprobe('{func}', false)")

        assert len(conn.execute("select * from list_uprobes()")) == 0


def test_multi_HIST_shared_uprobes(node: PostgresNode):
     with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "HIST", True)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:

            conn.execute(f"select delete_uprobe('{func}', true)")
            sleep(0.2) #just in case
            file_lines = node_read_file(node, f"/pg_uprobe/HIST_{func}.txt")

            check_HIST_uprobe_file(file_lines)


def test_multi_MEM_shared_uprobe(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "MEM", True)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:

            conn.execute(f"select delete_uprobe('{func}', true)")
            sleep(0.2) #just in case
            file_lines = node_read_file(node, f"/pg_uprobe/MEM_{func}.txt")

            check_MEM_uprobe_file(file_lines)


def test_invalid_uprobe(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        has_error = False
        try:
            set_uprobe(conn, "12345", "HIST", True)
        except:
            has_error = True

        assert has_error, "there is no sucn function in Postgres(I hope)"

        has_error = False

        try:
            set_uprobe(conn, "PortalStart", "12345", True)
        except:
            has_error = True

        assert has_error, "there is no sucn uprobe type"


def test_invalid_stat_args(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, "PortalStart", "TIME", False)
        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalStart')")
        except:
            has_error = True

        assert has_error, "You can't stat TIME uprobe with stat_hist_uprobe function"

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


        set_uprobe(conn, "PortalStart", "HIST", False)
        has_error = False
        try:
            conn.execute("select * from stat_time_uprobe('PortalStart')")
        except:
            has_error = True

        assert has_error, "You can't stat HIST uprobe with stat_TIME_uprobe function"


        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalStart', 100.0, 0.0, 10.0)")
        except:
            has_error = True

        assert has_error, "You must get error if you pass value start grater than stop"

        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalStart', 0.0, 100.0, -10.0)")
        except:
            has_error = True

        assert has_error, "You must get error if you pass negative step value"

        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalStart', 0.0, 100.0, 0.0)")
        except:
            has_error = True

        assert has_error, "You must get error if you pass zero step value"

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


def test_invalid_uprobe_name(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalStart')")
        except:
            has_error = True

        assert has_error, "You shouldn't have uprobe on PortalStart at this point"

        has_error = False
        try:
            conn.execute("select * from stat_time_uprobe('PortalStart')")
        except:
            has_error = True

        assert has_error, "You shouldn't have uprobe on PortalStart at this point"

        set_uprobe(conn, "PortalStart", "TIME", False)

        has_error = False
        try:
            conn.execute("select * from stat_hist_uprobe('PortalRun')")
        except:
            has_error = True

        assert has_error, "You shouldn't have uprobe on PortalRun at this point"

        has_error = False
        try:
            conn.execute("select * from stat_time_uprobe('PortalRun')")
        except:
            has_error = True

        assert has_error, "You shouldn't have uprobe on PortalRun at this point"

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


def test_delete_donot_save(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "HIST", True)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:
            conn.execute(f"select delete_uprobe('{func}', false)")

            assert not node_has_file(node, f"/pg_uprobe/HIST_{func}.txt")


def test_stat_clear(node: PostgresNode):
    with node.connect("postgres", autocommit=True) as conn:
        for func in test_multy_functions:
            set_uprobe(conn, func, "TIME", True)

        assert len(conn.execute("select * from list_uprobes()")) == len(test_multy_functions)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        functions_reorder = test_multy_functions.copy()
        random.shuffle(functions_reorder)

        for func in functions_reorder:
            conn.execute(f"select dump_uprobe_stat('{func}', true)")
            sleep(0.2)
            file_lines = node_read_file(node, f"/pg_uprobe/TIME_{func}.txt")
            first_stat = TIME_uprobe_get_stat(file_lines)

            conn.execute(f"select dump_uprobe_stat('{func}', true)")
            sleep(0.2)
            file_lines = node_read_file(node, f"/pg_uprobe/TIME_{func}.txt")
            second_stat = TIME_uprobe_get_stat(file_lines)

            assert first_stat[0] > second_stat[0] or first_stat[0] == 0, "There must be less call in stat after stat drop or there we non in the first place"


        node.pgbench_run(time=5, client=10, jobs=5, builtin="select-only")


        prev_stat = []
        post_stat = []

        for func in functions_reorder:
            conn.execute(f"select dump_uprobe_stat('{func}', false)")
            sleep(0.2)
            file_lines = node_read_file(node, f"/pg_uprobe/TIME_{func}.txt")
            prev_stat.append(TIME_uprobe_get_stat(file_lines))

        node.pgbench_run(time=5, client=10, jobs=5, builtin="select-only")


        for func in functions_reorder:
            conn.execute(f"select dump_uprobe_stat('{func}', false)")
            sleep(0.2)
            file_lines = node_read_file(node, f"/pg_uprobe/TIME_{func}.txt")
            post_stat.append(TIME_uprobe_get_stat(file_lines))

        for i in range(len(functions_reorder)):
            assert prev_stat[i][0] <= post_stat[i][0], "We can't lose any calls if we don't drop stat"

        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


def test_data_dir_change(node: PostgresNode):
    Path("/tmp/test_session_trace_dir").mkdir(parents=True, exist_ok=True)
    node.execute("alter system set pg_uprobe.data_dir to '/tmp/test_session_trace_dir'")
    node.restart()

    with node.connect("postgres", autocommit=True) as conn:
        set_uprobe(conn, 'PortalStart', 'TIME', True)

        node.pgbench_run(time=10, client=10, jobs=5, builtin="select-only")

        dump_uprobe_stat(conn, 'PortalStart', True)
        result = open("/tmp/test_session_trace_dir/TIME_PortalStart.txt").readlines()

        check_TIME_uprobe_file(result, True)
        conn.execute("select delete_uprobe(func, false) from list_uprobes()")


def run_tests(node: PostgresNode):
        test_wrapper(node, test_TIME_urpobe_local)
        test_wrapper(node, test_HIST_uprobe_local)

        test_wrapper(node, test_TIME_uprobe_shared)
        test_wrapper(node, test_HIST_uprobe_shared)
        test_wrapper(node, test_MEM_uprobe_shared)

        test_wrapper(node, test_multy_TIME_local_uprobes)
        test_wrapper(node, test_multi_HIST_local_uprobes)

        test_wrapper(node, test_multi_TIME_shared_uprobes)
        test_wrapper(node, test_multi_HIST_shared_uprobes)
        test_wrapper(node, test_multi_MEM_shared_uprobe)

        test_wrapper(node, test_invalid_uprobe)
        test_wrapper(node, test_invalid_stat_args)
        test_wrapper(node, test_invalid_uprobe_name)
        test_wrapper(node, test_delete_donot_save)
        test_wrapper(node, test_stat_clear)

        test_wrapper(node, test_data_dir_change)

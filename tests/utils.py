from testgres import PostgresNode
import os
import re
global_test_variable = 1


def node_read_file(node: PostgresNode, path : str) -> list[str]:
    file = open(node.data_dir + path)
    result = file.readlines()
    file.close()
    return result


def node_read_file_one_line(node: PostgresNode, path: str):
    file = open(node.data_dir + path)
    result = file.read(-1)
    file.close()
    return result


def node_delete_pg_uprobe_files(node: PostgresNode):
    dirEntries = os.listdir(node.data_dir + "/pg_uprobe/")
    for file in dirEntries:
        os.remove(node.data_dir + "/pg_uprobe/" + file)


def save_to_file_with_pid(data: str, pid: int):
    file = open(f"data_{pid}.txt", "w")
    file.write(data)
    file.close()


def test_wrapper(node: PostgresNode, test):
    global global_test_variable
    print("test", global_test_variable, ": ", test.__qualname__, ':', end = ' ', flush=True)
    global_test_variable += 1
    try:
        test(node)
        print("done")
    finally:
        node.execute("select delete_uprobe(func, false) from list_uprobes()")
        node_delete_pg_uprobe_files(node)
        node.execute("alter system set pg_uprobe.data_dir to DEFAULT")
        node.restart()


def node_get_file_size(node: PostgresNode, path: str):
    return os.stat(node.data_dir + path).st_size


def load_script(script_name: str):
    file = open(os.path.dirname(__file__) + "/scripts/" + script_name)
    result = file.read(-1)
    file.close()
    return result


def load_scripts():
    return [load_script("query_" + str(i) + ".sql") for i in range(1, 6)]



def node_load_custom_database(node: PostgresNode):
    node.psql(query = load_script("init.sql"))
    node.psql(query = load_script("data_load.sql"))


def extract_numbers_as_strings_from_time_point(input_string:str):
    date_part, time_part = input_string.split('T')
    year, month, day = date_part.split(':')
    hours, minutes, seconds = time_part.split(':')
    seconds, milliseconds = seconds.split('.')
    return [year, month, day, hours, minutes, seconds, milliseconds]

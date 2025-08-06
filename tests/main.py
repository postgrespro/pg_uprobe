import testgres
from testgres import PostgresNode

import test_functions_profile
import test_trace_session
import utils

def test_prepare(node: PostgresNode):
    node.pgbench_init(scale = "20")
    utils.node_load_custom_database(node)
    with node.connect("postgres", autocommit=True) as conn:
        conn.execute("create extension pg_uprobe")


def run_tests():
    with testgres.get_new_node().init() as node:
        node.append_conf("postgresql.conf", "shared_preload_libraries = 'pg_uprobe'")

        node.start()

        test_prepare(node)

        test_functions_profile.run_tests(node)

        test_trace_session.run_tests(node)




if __name__ == '__main__':
    run_tests()
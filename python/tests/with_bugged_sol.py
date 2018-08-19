#!/usr/bin/env python3

from task_maker.tests.test import run_tests, TestingUI


def test_task():
    from task_maker.tests.utils import TestInterface
    interface = TestInterface("with_bugged_sol", "Testing task-maker", 1,
                              65536)
    interface.set_generator("generatore.py")
    interface.set_generation_errors("No buono")
    interface.set_fatal_error()
    interface.run_checks(TestingUI.inst)


if __name__ == "__main__":
    run_tests("with_bugged_sol", __file__)

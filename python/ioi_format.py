#!/usr/bin/env python

from __future__ import print_function

import argparse
import glob
import os
from typing import Dict, List, Any, Tuple
from typing import Optional

import yaml
from proto.manager_pb2 import EvaluateTaskRequest
from proto.task_pb2 import Dependency
from proto.task_pb2 import GraderInfo
from proto.task_pb2 import SUM, MIN  # ScoreMode
from proto.task_pb2 import Subtask
from proto.task_pb2 import Task
from proto.task_pb2 import TestCase

from python.absolutize import absolutize_request
from python.dependency_finder import find_dependency
from python.language import grader_from_file, valid_extensions
from python.sanitize import sanitize_command
from python.source_file import from_file


def list_files(patterns, exclude=None):
    # type: (List[str], Optional[List[str]]) -> List[str]
    if exclude is None:
        exclude = []
    files = [_file for pattern in patterns
             for _file in glob.glob(pattern)]  # type: List[str]
    return [
        res for res in files
        if res not in exclude
           and os.path.splitext(res)[1] in valid_extensions()
    ]


def load_testcases():
    # type: () -> Tuple[Optional[str], Dict[int, Subtask]]
    nums = [
        int(input_file[11:-4])
        for input_file in glob.glob(os.path.join("input", "input*.txt"))
    ]
    if not nums:
        raise RuntimeError("No generator and no input files found!")

    subtask = Subtask()
    subtask.score_mode = SUM
    subtask.max_score = 100

    for num in sorted(nums):
        testcase = TestCase()
        testcase.input_file = os.path.join("input", "input%d.txt" % num)
        testcase.output_file = os.path.join("output", "output%d.txt" % num)
        subtask.testcases[num].CopyFrom(testcase)
    return None, {0: subtask}


def get_generator():
    # type: () -> Optional[str]
    for generator in list_files(["gen/generator.*", "gen/generatore.*"]):
        return generator
    return None


def gen_testcases(copy_compiled):
    # type: (bool) -> Tuple[Optional[str], Dict[int, Subtask]]
    validator = None  # type: Optional[str]
    subtasks = {}  # type: Dict[int, Subtask]
    official_solution = None  # type: Optional[str]

    def create_subtask(subtask_num, testcases, score):
        # type: (int, Dict[int, TestCase], float) -> None
        if testcases:
            subtask = Subtask()
            subtask.score_mode = MIN
            subtask.max_score = score
            for testcase_num, testcase in testcases.items():
                subtask.testcases[testcase_num].CopyFrom(testcase)
            subtasks[subtask_num] = subtask

    generator = get_generator()
    if not generator:
        return load_testcases()
    for _validator in list_files(["gen/validator.*", "gen/valida.*"]):
        validator = _validator
    if not validator:
        raise RuntimeError("No validator found")
    for solution in list_files(["sol/solution.*", "sol/soluzione.*"]):
        official_solution = solution
    if official_solution is None:
        raise RuntimeError("No official solution found")

    current_testcases = {}  # type: Dict[int, TestCase]
    subtask_num = -1  # the first #ST line will skip a subtask!
    testcase_num = 0
    current_score = 0.0
    for line in open("gen/GEN"):
        testcase = TestCase()
        if line.startswith("#ST: "):
            create_subtask(subtask_num, current_testcases, current_score)
            subtask_num += 1
            current_testcases = {}
            current_score = float(line.strip()[5:])
            continue
        elif line.startswith("#COPY: "):
            testcase.input_file = line[7:].strip()
        else:
            line = line.split("#")[0].strip()
            if not line:
                continue
            args = line.split()
            arg_deps = sanitize_command(args)
            testcase.generator.CopyFrom(
                from_file(generator, copy_compiled and "bin/generator"))
            testcase.args.extend(args)
            testcase.extra_deps.extend(arg_deps)
            testcase.validator.CopyFrom(
                from_file(validator, copy_compiled and "bin/validator"))
        current_testcases[testcase_num] = testcase
        testcase_num += 1

    # if the task has no subtasks, the starting number should be 0
    if subtask_num == -1:
        subtask_num = 0
    create_subtask(subtask_num, current_testcases, current_score)
    # Hack for when subtasks are not specified.
    if len(subtasks) == 1 and subtasks[0].max_score == 0:
        subtasks[0].score_mode = SUM
        subtasks[0].max_score = 100
    return official_solution, subtasks


def detect_yaml():
    # type: () -> str
    cwd = os.getcwd()
    task_name = os.path.basename(cwd)
    yaml_names = ["task", os.path.join("..", task_name)]
    yaml_ext = ["yaml", "yml"]
    for name in yaml_names:
        for ext in yaml_ext:
            path = os.path.join(cwd, name + "." + ext)
            if os.path.exists(path):
                return path
    raise IOError("Cannot find the task yaml of %s" % cwd)


def parse_task_yaml():
    # type: () -> Dict[str, Any]
    path = detect_yaml()
    with open(path) as yaml_file:
        return yaml.load(yaml_file)


def get_options(data, names, default=None):
    # type: (Dict[str, Any], List[str], Optional[Any]) -> Any
    for name in names:
        if name in data:
            return data[name]
    if not default:
        raise ValueError("Non optional field %s missing from task.yaml"
                         % "|".join(names))
    return default


def create_task_from_yaml(data):
    # type: (Dict[str, Any]) -> Task
    name = get_options(data, ["name", "nome_breve"])
    title = get_options(data, ["title", "nome"])
    if name is None:
        raise ValueError("The name is not set in the yaml")
    if title is None:
        raise ValueError("The title is not set in the yaml")

    time_limit = get_options(data, ["time_limit", "timeout"])
    memory_limit = get_options(data, ["memory_limit", "memlimit"]) * 1024
    input_file = get_options(data, ["infile"], "input.txt")
    output_file = get_options(data, ["outfile"], "output.txt")

    task = Task()
    task.name = name
    task.title = title
    task.time_limit = time_limit
    task.memory_limit_kb = memory_limit
    task.input_file = input_file if input_file else ""
    task.output_file = output_file if output_file else ""
    return task


def get_request(args):
    # type: (argparse.Namespace) -> EvaluateTaskRequest
    copy_compiled = args.copy_exe
    data = parse_task_yaml()
    if not data:
        raise RuntimeError("The task.yaml is not valid")

    task = create_task_from_yaml(data)

    graders = list_files(["sol/grader.*"])
    if args.solutions:
        solutions = [
            sol if sol.startswith("sol/") else "sol/" + sol
            for sol in args.solutions
        ]
    else:
        solutions = list_files(
            ["sol/*"], exclude=graders + ["sol/__init__.py"])

    checkers = list_files(["cor/checker.*", "cor/correttore.cpp"])
    if not checkers:
        checker = None
    elif len(checkers) == 1:
        checker = checkers[0]
    else:
        raise ValueError("Too many checkers in cor/ folder")

    official_solution, subtasks = gen_testcases(copy_compiled)
    if official_solution:
        task.official_solution.CopyFrom(
            from_file(official_solution,
                      copy_compiled and "bin/official_solution"))

    if checker is not None:
        task.checker.CopyFrom(
            from_file(checker, copy_compiled and "bin/checker"))
    for grader in graders:
        info = GraderInfo()
        info.for_language = grader_from_file(grader)
        name = os.path.basename(grader)
        info.files.extend([Dependency(name=name, path=grader)] +
                          find_dependency(grader))
        task.grader_info.extend([info])
    for subtask_num, subtask in subtasks.items():
        task.subtasks[subtask_num].CopyFrom(subtask)
    num_testcases = sum(len(subtask.testcases) for subtask in subtasks.values())

    request = EvaluateTaskRequest()
    request.task.CopyFrom(task)
    for solution in solutions:
        bin_file = copy_compiled and "bin/" + \
                   os.path.splitext(os.path.basename(solution))[0]
        request.solutions.extend([from_file(solution, bin_file)])
    request.store_dir = args.store_dir
    request.temp_dir = args.temp_dir
    request.keep_sandbox = args.keep_sandbox
    for testcase in range(num_testcases):
        request.write_inputs_to[testcase] = "input/input%d.txt" % testcase
        request.write_outputs_to[testcase] = "output/output%d.txt" % testcase
    request.write_checker_to = "cor/checker"
    request.cache_mode = args.cache
    if args.num_cores:
        request.num_cores = args.num_cores
    request.dry_run = args.dry_run
    if args.evaluate_on:
        request.evaluate_on = args.evaluate_on
    absolutize_request(request)
    return request


def clean():
    def remove_dir(path, pattern):
        # type: (str, str) -> None
        if not os.path.exists(path):
            return
        for file in glob.glob(os.path.join(path, pattern)):
            os.remove(file)
        try:
            os.rmdir(path)
        except OSError:
            print("Directory %s not empty, kept non-%s files" % (path, pattern))

    def remove_file(path):
        # type: (str) -> None
        try:
            os.remove(path)
        except OSError:
            pass

    if get_generator():
        remove_dir("input", "*.txt")
        remove_dir("output", "*.txt")
    remove_dir("bin", "*")
    remove_file(os.path.join("cor", "checker"))
    remove_file(os.path.join("cor", "correttore"))
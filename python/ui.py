#!/usr/bin/env python3
import time

from enum import Enum
from task_maker.formats import Task, ScoreMode
from task_maker.printer import StdoutPrinter, Printer
from typing import List, Dict

from task_maker.source_file import SourceFile
from task_maker.task_maker_frontend import Execution, Result, ResultStatus


class TestcaseGenerationStatus(Enum):
    WAITING = 0
    GENERATING = 1
    GENERATED = 2
    VALIDATING = 3
    VALIDATED = 4
    SOLVING = 5
    DONE = 6
    FAILURE = 7


class SourceFileCompilationStatus(Enum):
    WAITING = 0
    COMPILING = 1
    DONE = 2
    FAILURE = 3


class TestcaseSolutionResult(Enum):
    WAITING = 0
    SOLVING = 1
    SOLVED = 2
    CHECKING = 3
    ACCEPTED = 4
    WRONG_ANSWER = 5
    SIGNAL = 6
    RETURN_CODE = 7
    TIME_LIMIT = 8
    WALL_LIMIT = 9
    MEMORY_LIMIT = 10
    MISSING_FILES = 11
    INTERNAL_ERROR = 12
    SKIPPED = 13


class SubtaskSolutionResult(Enum):
    WAITING = 0
    ACCEPTED = 1
    PARTIAL = 2
    REJECTED = 3


class SolutionStatus:
    def __init__(self, source_file: SourceFile, task: Task,
                 subtasks: Dict[int, List[int]]):
        self.source_file = source_file
        self.task = task
        self.score = 0.0
        self.subtask_scores = dict((st_num, 0.0) for st_num in subtasks)
        self.subtask_results = [SubtaskSolutionResult.WAITING] * len(subtasks)
        self.testcase_results = dict(
        )  # type: Dict[int, Dict[int, TestcaseSolutionResult]]
        self.testcase_scores = dict()
        self.st_remaining_cases = [
            len(subtask) for subtask in subtasks.values()
        ]

        for st_num, subtask in subtasks.items():
            self.testcase_results[st_num] = dict()
            self.testcase_scores[st_num] = dict()
            for tc_num in subtask:
                self.testcase_results[st_num][
                    tc_num] = TestcaseSolutionResult.WAITING
                self.testcase_scores[st_num][tc_num] = 0.0

    def update_eval_result(self, subtask: int, testcase: int, result: Result):
        if result.status == ResultStatus.SIGNAL:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.SIGNAL
        elif result.status == ResultStatus.RETURN_CODE:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.RETURN_CODE
        elif result.status == ResultStatus.TIME_LIMIT:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.TIME_LIMIT
        elif result.status == ResultStatus.WALL_LIMIT:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.WALL_LIMIT
        elif result.status == ResultStatus.MEMORY_LIMIT:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.MEMORY_LIMIT
        elif result.status == ResultStatus.MISSING_FILES:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.MISSING_FILES
        elif result.status == ResultStatus.INTERNAL_ERROR:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.INTERNAL_ERROR
        else:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.SOLVED
        if result.status != ResultStatus.SUCCESS:
            self.st_remaining_cases[subtask] -= 1
            if self.st_remaining_cases[subtask] == 0:
                self._compute_st_score(subtask)
        # TODO store used resources

    def update_check_result(self, subtask: int, testcase: int, result: Result):
        if result.status == ResultStatus.SIGNAL:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.INTERNAL_ERROR
        elif result.status == ResultStatus.INTERNAL_ERROR:
            self.testcase_results[subtask][
                testcase] = TestcaseSolutionResult.INTERNAL_ERROR
        # TODO check if the task has a custom checker
        else:
            if result.status == ResultStatus.SUCCESS:
                self.testcase_scores[subtask][testcase] = 1.0
                self.testcase_results[subtask][
                    testcase] = TestcaseSolutionResult.ACCEPTED
            else:
                self.testcase_scores[subtask][testcase] = 0.0
                self.testcase_results[subtask][
                    testcase] = TestcaseSolutionResult.WRONG_ANSWER
            self.st_remaining_cases[subtask] -= 1
            if self.st_remaining_cases[subtask] == 0:
                self._compute_st_score(subtask)

    def _compute_st_score(self, subtask: int):
        scores = self.testcase_scores[subtask].values()
        score_mode = self.task.subtasks[subtask].score_mode
        if score_mode == ScoreMode.MIN:
            score = min(scores)
        elif score_mode == ScoreMode.MAX:
            score = max(scores)
        elif score_mode == ScoreMode.SUM:
            score = sum(scores) / len(scores)
        else:
            raise ValueError("Invalid score mode", score_mode)
        score *= self.task.subtasks[subtask].max_score
        self.subtask_scores[subtask] = score
        self.score = sum(self.subtask_scores.values())
        if min(scores) == 1.0:
            self.subtask_results[subtask] = SubtaskSolutionResult.ACCEPTED
        elif score == 0.0:
            self.subtask_results[subtask] = SubtaskSolutionResult.REJECTED
        else:
            self.subtask_results[subtask] = SubtaskSolutionResult.PARTIAL


class IOILikeUIInterface:
    def __init__(self, task: Task, testcases: Dict[int, List[int]],
                 do_print: bool):
        self.task = task
        self.subtasks = dict(
        )  # type: Dict[int, Dict[int, TestcaseGenerationStatus]]
        self.testcases = testcases
        self.non_solutions = dict(
        )  # type: Dict[str, SourceFileCompilationStatus]
        self.solutions = dict()  # type: Dict[str, SourceFileCompilationStatus]
        self.testing = dict()  # type: Dict[str, SolutionStatus]
        self.running = dict()  # type: Dict[str, float]
        if do_print:
            self.printer = StdoutPrinter()
        else:
            self.printer = Printer()

        for st_num, subtask in testcases.items():
            self.subtasks[st_num] = dict()
            for tc_num in subtask:
                self.subtasks[st_num][
                    tc_num] = TestcaseGenerationStatus.WAITING

    def add_non_solution(self, source_file: SourceFile):
        name = source_file.name
        log_prefix = "Compilation of non-solution {} ".format(name).ljust(50)
        self.non_solutions[name] = SourceFileCompilationStatus.WAITING
        self.printer.text(log_prefix + "WAITING\n")
        if source_file.need_compilation:

            def notifyStartCompiltion():
                self.printer.text(log_prefix + "START\n")
                self.non_solutions[
                    name] = SourceFileCompilationStatus.COMPILING
                self.running[log_prefix] = time.monotonic()

            def getResultCompilation(result: Result):
                del self.running[log_prefix]
                if result.status == ResultStatus.SUCCESS:
                    self.printer.green(log_prefix + "SUCCESS\n")
                    self.non_solutions[name] = SourceFileCompilationStatus.DONE
                else:
                    self.printer.red(log_prefix +
                                     "FAIL: {}\n".format(result.status))
                    # TODO: write somewhere why
                    self.non_solutions[
                        name] = SourceFileCompilationStatus.FAILURE

            source_file.compilation.notifyStart(notifyStartCompiltion)
            source_file.compilation.getResult(getResultCompilation)
        else:
            self.printer.green(log_prefix + "SUCCESS\n")
            self.non_solutions[name] = SourceFileCompilationStatus.DONE

    def add_solution(self, source_file: SourceFile):
        name = source_file.name
        log_prefix = "Compilation of solution {} ".format(name).ljust(50)
        self.solutions[name] = SourceFileCompilationStatus.WAITING
        self.testing[name] = SolutionStatus(source_file, self.task,
                                            self.testcases)
        self.printer.text(log_prefix + "WAITING\n")

        if source_file.need_compilation:

            def notifyStartCompiltion():
                self.printer.text(log_prefix + "START\n")
                self.solutions[name] = SourceFileCompilationStatus.COMPILING
                self.running[log_prefix] = time.monotonic()

            def getResultCompilation(result: Result):
                del self.running[log_prefix]
                if result.status == ResultStatus.SUCCESS:
                    self.printer.green(log_prefix + "SUCCESS\n")
                    self.solutions[name] = SourceFileCompilationStatus.DONE
                else:
                    self.printer.red(log_prefix +
                                     "FAIL: {}\n".format(result.status))
                    # TODO: write somewhere why
                    self.solutions[name] = SourceFileCompilationStatus.FAILURE

            source_file.compilation.notifyStart(notifyStartCompiltion)
            source_file.compilation.getResult(getResultCompilation)
        else:
            self.printer.green(log_prefix + "SUCCESS\n")
            self.solutions[name] = SourceFileCompilationStatus.DONE

    def add_generation(self, subtask: int, testcase: int,
                       generation: Execution):
        log_prefix = "Generation of input {} of subtask {} ".format(
            testcase, subtask).ljust(50)
        self.printer.text(log_prefix + "WAITING\n")

        def notifyStartGeneration():
            self.printer.text(log_prefix + "START\n")
            self.subtasks[subtask][
                testcase] = TestcaseGenerationStatus.GENERATING
            self.running[log_prefix] = time.monotonic()

        def getResultGeneration(result: Result):
            del self.running[log_prefix]
            if result.status == ResultStatus.SUCCESS:
                self.printer.green(log_prefix + "SUCCESS\n")
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.GENERATED
            else:
                self.printer.red(log_prefix +
                                 "FAIL: {}\n".format(result.status))
                # TODO: write somewhere why
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.FAILURE

        def skippedGeneration():
            self.printer.red(log_prefix + "SKIPPED\n")

        generation.notifyStart(notifyStartGeneration)
        generation.getResult(getResultGeneration, skippedGeneration)

    def add_validation(self, subtask: int, testcase: int,
                       validation: Execution):
        log_prefix = "Validation of input {} of subtask {} ".format(
            testcase, subtask).ljust(50)
        self.printer.text(log_prefix + "WAITING\n")

        def notifyStartValidation():
            self.printer.text(log_prefix + "START\n")
            self.subtasks[subtask][
                testcase] = TestcaseGenerationStatus.VALIDATING
            self.running[log_prefix] = time.monotonic()

        def getResultValidation(result: Result):
            del self.running[log_prefix]
            if result.status == ResultStatus.SUCCESS:
                self.printer.green(log_prefix + "SUCCESS\n")
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.VALIDATED
            else:
                self.printer.red(log_prefix +
                                 "FAIL: {}\n".format(result.status))
                # TODO: write somewhere why
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.FAILURE

        def skippedValidation():
            self.printer.red(log_prefix + "SKIPPED\n")

        validation.notifyStart(notifyStartValidation)
        validation.getResult(getResultValidation, skippedValidation)

    def add_solving(self, subtask: int, testcase: int, solving: Execution):
        log_prefix = "Generation of output {} of subtask {} ".format(
            testcase, subtask).ljust(50)
        self.printer.text(log_prefix + "WAITING\n")

        def notifyStartSolving():
            self.printer.text(log_prefix + "START\n")
            self.subtasks[subtask][testcase] = TestcaseGenerationStatus.SOLVING
            self.running[log_prefix] = time.monotonic()

        def getResultSolving(result: Result):
            del self.running[log_prefix]
            if result.status == ResultStatus.SUCCESS:
                self.printer.green(log_prefix + "SUCCESS\n")
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.DONE
            else:
                self.printer.red(log_prefix +
                                 "FAIL: {}\n".format(result.status))
                # TODO: write somewhere why
                self.subtasks[subtask][
                    testcase] = TestcaseGenerationStatus.FAILURE

        def skippedSolving():
            self.printer.red(log_prefix + "SKIPPED\n")

        solving.notifyStart(notifyStartSolving)
        solving.getResult(getResultSolving, skippedSolving)

    def add_evaluate_solution(self, subtask: int, testcase: int, solution: str,
                              evaluation: Execution):
        log_prefix = "Evaluate {} on case {} ".format(solution,
                                                      testcase).ljust(50)
        self.printer.text(log_prefix + "WAITING\n")

        def notifyStartEvaluation():
            self.printer.text(log_prefix + "START\n")
            self.testing[solution].testcase_results[subtask][
                testcase] = TestcaseSolutionResult.SOLVING
            self.running[log_prefix] = time.monotonic()

        def getResultEvaluation(result: Result):
            del self.running[log_prefix]
            if result.status == ResultStatus.SUCCESS:
                self.printer.green(log_prefix + "SUCCESS\n")
            else:
                self.printer.red(log_prefix +
                                 "FAIL: {}\n".format(result.status))

            self.testing[solution].update_eval_result(subtask, testcase,
                                                      result)

        def skippedEvaluation():
            self.testing[solution].testcase_results[subtask][
                testcase] = TestcaseSolutionResult.SKIPPED
            self.printer.yellow(log_prefix + "SKIPPED\n")

        evaluation.notifyStart(notifyStartEvaluation)
        evaluation.getResult(getResultEvaluation, skippedEvaluation)

    def add_evaluate_checking(self, subtask: int, testcase: int, solution: str,
                              checking: Execution):
        log_prefix = "Checking {} on case {} ".format(solution,
                                                      testcase).ljust(50)
        self.printer.text(log_prefix + "WAITING\n")

        def notifyStartChecking():
            self.printer.text(log_prefix + "START\n")
            self.testing[solution].testcase_results[subtask][
                testcase] = TestcaseSolutionResult.CHECKING
            self.running[log_prefix] = time.monotonic()

        def getResultChecking(result: Result):
            del self.running[log_prefix]
            if result.status == ResultStatus.SUCCESS:
                self.printer.green(log_prefix + "SUCCESS\n")
            else:
                self.printer.red(log_prefix +
                                 "FAIL: {}\n".format(result.status))
            self.testing[solution].update_check_result(subtask, testcase,
                                                       result)

        def skippedChecking():
            self.printer.yellow(log_prefix + "SKIPPED\n")

        checking.notifyStart(notifyStartChecking)
        checking.getResult(getResultChecking, skippedChecking)

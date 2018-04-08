#ifndef MANAGER_EVENT_QUEUE_HPP
#define MANAGER_EVENT_QUEUE_HPP

#include <queue>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "proto/event.pb.h"
#include "proto/manager.grpc.pb.h"

namespace manager {

class EventQueue {
 public:
  void FatalError(const std::string& message) {
    proto::Event event;
    event.mutable_fatal_error()->set_msg(message);
    Enqueue(std::move(event));
  }
  void TaskScore(const std::string& solution, float score) {
    proto::Event event;
    auto* sub_event = event.mutable_task_score();
    sub_event->set_solution(solution);
    sub_event->set_score(score);
    Enqueue(std::move(event));
  }
  void SubtaskTaskScore(const std::string& solution, float score,
                        int64_t subtask_id) {
    proto::Event event;
    auto* sub_event = event.mutable_subtask_score();
    sub_event->set_solution(solution);
    sub_event->set_score(score);
    sub_event->set_subtask_id(subtask_id);
    Enqueue(std::move(event));
  }
  void CompilationWaiting(const std::string& filename) {
    Compilation(filename, proto::EventStatus::WAITING);
  }
  void CompilationRunning(const std::string& filename) {
    Compilation(filename, proto::EventStatus::RUNNING);
  }
  void CompilationDone(const std::string& filename, const std::string& errors,
                       bool from_cache) {
    Compilation(filename, proto::EventStatus::DONE, errors, from_cache);
  }
  void CompilationFailure(const std::string& filename,
                          const std::string& errors, bool from_cache) {
    Compilation(filename, proto::EventStatus::FAILURE, errors, from_cache);
  }
  void GenerationWaiting(int64_t testcase) {
    Generation(testcase, proto::EventStatus::WAITING);
  }
  void TerryGenerationWaiting(const std::string& solution) {
    TerryGeneration(solution, proto::EventStatus::WAITING);
  }
  void Generating(int64_t testcase) {
    Generation(testcase, proto::EventStatus::GENERATING);
  }
  void TerryGenerating(const std::string& solution) {
    TerryGeneration(solution, proto::EventStatus::GENERATING);
  }
  void Generated(int64_t testcase, bool from_cache) {
    Generation(testcase, proto::EventStatus::GENERATED, "", from_cache);
  }
  void TerryGenerated(const std::string& solution, bool from_cache) {
    TerryGeneration(solution, proto::EventStatus::GENERATED, "", from_cache);
  }
  void Validating(int64_t testcase) {
    Generation(testcase, proto::EventStatus::VALIDATING);
  }
  void TerryValidating(const std::string& solution) {
    TerryGeneration(solution, proto::EventStatus::VALIDATING);
  }
  void Validated(int64_t testcase, bool from_cache) {
    Generation(testcase, proto::EventStatus::VALIDATED, "", from_cache);
  }
  void TerryValidated(const std::string& solution, bool from_cache) {
    TerryGeneration(solution, proto::EventStatus::VALIDATED, "", from_cache);
  }
  void Solving(int64_t testcase) {
    Generation(testcase, proto::EventStatus::SOLVING);
  }
  void GenerationDone(int64_t testcase, bool from_cache) {
    Generation(testcase, proto::EventStatus::DONE, "", from_cache);
  }
  void GenerationFailure(int64_t testcase, const std::string& errors,
                         bool from_cache) {
    Generation(testcase, proto::EventStatus::FAILURE, errors, from_cache);
  }
  void TerryGenerationFailure(const std::string& solution,
                              const std::string& errors, bool from_cache) {
    TerryGeneration(solution, proto::EventStatus::FAILURE, errors, from_cache);
  }
  void EvaluationWaiting(const std::string& solution, int64_t testcase) {
    Evaluation(solution, testcase, proto::EventStatus::WAITING);
  }
  void Executing(const std::string& solution, int64_t testcase) {
    Evaluation(solution, testcase, proto::EventStatus::EXECUTING);
  }
  void TerryEvaluating(const std::string& solution) {
    TerryEvaluation(solution, proto::EventStatus::EXECUTING);
  }
  void Executed(const std::string& solution, int64_t testcase,
                bool from_cache) {
    Evaluation(solution, testcase, proto::EventStatus::EXECUTED, {},
               from_cache);
  }
  void TerryEvaluated(const std::string& solution, bool from_cache) {
    TerryEvaluation(solution, proto::EventStatus::EXECUTED, "", from_cache);
  }
  void Checking(const std::string& solution, int64_t testcase) {
    Evaluation(solution, testcase, proto::EventStatus::CHECKING);
  }
  void TerryChecking(const std::string& solution) {
    TerryCheck(solution, proto::EventStatus::CHECKING);
  }
  void TerryChecked(const std::string& solution,
                    proto::TerryEvaluationResult result, bool from_cache) {
    TerryCheck(solution, proto::EventStatus::DONE, "", std::move(result),
               from_cache);
  }
  void TerryCheckingFailure(const std::string& solution,
                            const std::string& errors, bool from_cache) {
    TerryCheck(solution, proto::EventStatus::FAILURE, errors, {}, from_cache);
  }
  void EvaluationDone(const std::string& solution, int64_t testcase,
                      float score, const std::string& message, float cpu_time,
                      float wall_time, int64_t memory, bool from_cache) {
    proto::EvaluationResult result;
    result.set_score(score);
    result.set_message(message);
    result.set_cpu_time_used(cpu_time);
    result.set_wall_time_used(wall_time);
    result.set_memory_used_kb(memory);
    Evaluation(solution, testcase, proto::EventStatus::DONE, std::move(result),
               from_cache);
  }
  void EvaluationFailure(const std::string& solution, int64_t testcase,
                         const std::string& message, float cpu_time,
                         float wall_time, int64_t memory, bool from_cache) {
    proto::EvaluationResult result;
    result.set_message(message);
    result.set_cpu_time_used(cpu_time);
    result.set_wall_time_used(wall_time);
    result.set_memory_used_kb(memory);
    Evaluation(solution, testcase, proto::EventStatus::FAILURE,
               std::move(result), from_cache);
  }
  void TerryEvaluationFailure(const std::string& solution,
                              const std::string& errors, bool from_cache) {
    TerryEvaluation(solution, proto::EventStatus::FAILURE, errors, from_cache);
  }

  void BindWriter(grpc::ServerWriter<proto::Event>* writer, std::mutex* mutex);
  void BindWriterUnlocked(grpc::ServerWriter<proto::Event>* writer);
  void Enqueue(proto::Event&& event);
  absl::optional<proto::Event> Dequeue();
  void Stop();
  bool IsStopped() { return stopped_; }

 private:
  absl::Mutex queue_mutex_;
  std::queue<proto::Event> queue_ GUARDED_BY(queue_mutex_);
  bool stopped_ GUARDED_BY(queue_mutex_) = false;
  void Compilation(const std::string& filename, proto::EventStatus status,
                   const std::string& errors = "", bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_compilation();
    sub_event->set_filename(filename);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (!errors.empty()) {
      sub_event->set_stderr(errors);
    }
    Enqueue(std::move(event));
  }
  void Generation(int64_t testcase, proto::EventStatus status,
                  const std::string& errors = "", bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_generation();
    sub_event->set_testcase(testcase);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (!errors.empty()) {
      sub_event->set_error(errors);
    }
    Enqueue(std::move(event));
  }
  void TerryGeneration(const std::string& solution, proto::EventStatus status,
                       const std::string& errors = "",
                       bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_terry_generation();
    sub_event->set_solution(solution);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (!errors.empty()) sub_event->set_error(errors);
    Enqueue(std::move(event));
  }
  void Evaluation(const std::string& solution, int64_t testcase,
                  proto::EventStatus status,
                  absl::optional<proto::EvaluationResult>&& result = {},
                  bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_evaluation();
    sub_event->set_solution(solution);
    sub_event->set_testcase(testcase);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (result.has_value()) {
      sub_event->mutable_result()->Swap(&result.value());
    }
    Enqueue(std::move(event));
  }
  void TerryEvaluation(const std::string& solution, proto::EventStatus status,
                       const std::string& errors = "",
                       bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_terry_evaluation();
    sub_event->set_solution(solution);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (!errors.empty()) sub_event->set_errors(errors);
    Enqueue(std::move(event));
  }
  void TerryCheck(const std::string& solution, proto::EventStatus status,
                  const std::string& errors = "",
                  absl::optional<proto::TerryEvaluationResult>&& result = {},
                  bool from_cache = false) {
    proto::Event event;
    auto* sub_event = event.mutable_terry_check();
    sub_event->set_solution(solution);
    sub_event->set_status(status);
    sub_event->set_from_cache(from_cache);
    if (!errors.empty()) sub_event->set_errors(errors);
    if (result.has_value()) sub_event->mutable_result()->Swap(&result.value());
    Enqueue(std::move(event));
  }
};

}  // namespace manager

#endif

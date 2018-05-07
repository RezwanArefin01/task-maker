#include "executor/local_executor.hpp"
#include "absl/strings/str_join.h"
#include "glog/logging.h"
#include "util/file.hpp"

#include <cctype>

#include <algorithm>
#include <fstream>
#include <thread>

namespace {
bool IsIllegalChar(char c) { return c == '/' or c == '\0'; }
}  // namespace

namespace executor {

proto::Response LocalExecutor::Execute(
    const proto::Request& request, const RequestFileCallback& file_callback) {
  if (request.fifo_size() != 0) {
    throw std::logic_error("FIFOs are not implemented yet");
  }
  for (const auto& input : request.input()) {
    MaybeRequestFile(input, file_callback);
  }

  sandbox::ExecutionInfo result;
  util::TempDir tmp(temp_directory_);

  std::string cmdline = request.executable();
  if (!request.arg().empty())
    for (const std::string& arg : request.arg()) cmdline += " '" + arg + "'";

  if (request.keep_sandbox()) {
    tmp.Keep();
    std::ofstream cmdline_file(util::File::JoinPath(tmp.Path(), "command.txt"));
    cmdline_file << cmdline << std::endl;
  }

  VLOG(2) << "Executing:\n"
          << "\tCommand:        " << cmdline << "\n"
          << "\tInside sandbox: " << tmp.Path();

  std::string sandbox_dir = util::File::JoinPath(tmp.Path(), kBoxDir);
  util::File::MakeDirs(sandbox_dir);

  // Folder and arguments.
  sandbox::ExecutionOptions exec_options(sandbox_dir, request.executable());
  for (const std::string& arg : request.arg()) {
    exec_options.args.push_back(arg);
  }

  // Limits.
  // Scale up time limits to have a good margin for random occurrences.
  exec_options.cpu_limit_millis = request.resource_limit().cpu_time() * 1200;
  exec_options.wall_limit_millis = request.resource_limit().wall_time() * 1200;
  exec_options.memory_limit_kb = request.resource_limit().memory() * 1.2;
  exec_options.max_files = request.resource_limit().nfiles();
  exec_options.max_procs = request.resource_limit().processes();
  exec_options.max_file_size_kb = request.resource_limit().fsize();
  exec_options.max_mlock_kb = request.resource_limit().mlock();
  exec_options.max_stack_kb = request.resource_limit().stack();

  // Input files.
  bool loaded_executable = false;
  std::vector<std::string> input_files;
  for (const auto& input : request.input()) {
    PrepareFile(input, tmp.Path(), &exec_options, &input_files);
    if (input.name() == request.executable()) {
      loaded_executable = true;
      // Do not call MakeImmutable on the main executable, as
      // PrepareForExecution will take care of immutability either way and
      // doing so could cause race conditions because of hardlinks.
      input_files.pop_back();
    }
  }

  // Stdout/err files.
  exec_options.stdout_file = util::File::JoinPath(tmp.Path(), "stdout");
  exec_options.stderr_file = util::File::JoinPath(tmp.Path(), "stderr");

  std::string error_msg;
  std::unique_ptr<sandbox::Sandbox> sb = sandbox::Sandbox::Create();

  if (loaded_executable &&
      !sb->PrepareForExecution(
          util::File::JoinPath(sandbox_dir, request.executable()),
          &error_msg)) {
    throw std::runtime_error(error_msg);
  }
  // Actual execution.
  {
    ThreadGuard guard(/*exclusive = */ request.exclusive());
    if (!sb->Execute(exec_options, &result, &error_msg)) {
      throw std::runtime_error(error_msg);
    }
  }

  proto::Response response;

  // Resource usage.
  response.mutable_resource_usage()->set_cpu_time(result.cpu_time_millis /
                                                  1000.0);
  response.mutable_resource_usage()->set_sys_time(result.sys_time_millis /
                                                  1000.0);
  response.mutable_resource_usage()->set_wall_time(result.wall_time_millis /
                                                   1000.0);
  response.mutable_resource_usage()->set_memory(result.memory_usage_kb);

  // Termination status.
  response.set_status_code(result.status_code);
  response.set_signal(result.signal);
  if (request.resource_limit().memory() != 0 &&
      response.resource_usage().memory() >= request.resource_limit().memory()) {
    response.set_status(proto::Status::MEMORY_LIMIT);
    response.set_error_message("Memory limit exceeded");
  } else if (request.resource_limit().cpu_time() != 0 &&
             response.resource_usage().sys_time() +
                     response.resource_usage().cpu_time() >=
                 request.resource_limit().cpu_time()) {
    response.set_status(proto::Status::TIME_LIMIT);
    response.set_error_message("CPU limit exceeded");
  } else if (request.resource_limit().wall_time() != 0 &&
             response.resource_usage().wall_time() >=
                 request.resource_limit().wall_time()) {
    response.set_status(proto::Status::TIME_LIMIT);
    response.set_error_message("Wall limit exceeded");
  } else if (response.signal() != 0) {
    response.set_status(proto::Status::SIGNAL);
    response.set_error_message(result.message);
  } else if (response.status_code() != 0) {
    response.set_status(proto::Status::NONZERO);
    response.set_error_message(result.message);
  } else {
    response.set_status(proto::Status::SUCCESS);
  }

  // Output files.
  proto::FileInfo info;
  info.set_type(proto::FileType::STDOUT);
  RetrieveFile(info, tmp.Path(), &response);
  info.set_type(proto::FileType::STDERR);
  RetrieveFile(info, tmp.Path(), &response);
  for (const proto::FileInfo& info : request.output()) {
    try {
      RetrieveFile(info, tmp.Path(), &response);
    } catch (const std::system_error& exc) {
      if (exc.code().value() !=
          static_cast<int>(std::errc::no_such_file_or_directory))
        throw;
      if (response.status() == proto::Status::SUCCESS) {
        response.set_status(proto::Status::MISSING_FILES);
        response.set_error_message("Missing output files");
      }
    }
  }
  return response;
}

void LocalExecutor::PrepareFile(const proto::FileInfo& info,
                                const std::string& tmpdir,
                                sandbox::ExecutionOptions* options,
                                std::vector<std::string>* input_files) {
  std::string name = info.name();
  if (info.type() == proto::FileType::STDIN) {
    name = "stdin";
    options->stdin_file = util::File::JoinPath(tmpdir, name);
  } else {
    if (std::find_if(name.begin(), name.end(), IsIllegalChar) != name.end()) {
      throw std::runtime_error("Invalid file name: " + name);
    }
    name = util::File::JoinPath(kBoxDir, name);
  }
  std::string source_path =
      util::File::ProtoSHAToPath(store_directory_, info.hash());
  std::string target_path = util::File::JoinPath(tmpdir, name);
  util::File::Copy(source_path, target_path);
  if (info.executable())
    util::File::MakeExecutable(target_path);
  input_files->push_back(util::File::JoinPath(tmpdir, name));
}

void LocalExecutor::RetrieveFile(const proto::FileInfo& info,
                                 const std::string& tmpdir,
                                 proto::Response* options) {
  std::string name = info.name();
  if (info.type() == proto::FileType::STDOUT ||
      info.type() == proto::FileType::STDERR) {
    name =
        info.type() == proto::FileType::STDOUT ? "stdout" : "stderr";  // NOLINT
  } else {
    if (std::find_if(name.begin(), name.end(), IsIllegalChar) != name.end()) {
      throw std::runtime_error("Invalid file name");
    }
    name = util::File::JoinPath(kBoxDir, name);
  }
  util::SHA256_t hash = util::File::Hash(util::File::JoinPath(tmpdir, name));
  proto::FileInfo out_info = info;
  std::string destination_path = util::File::SHAToPath(store_directory_, hash);
  util::File::Copy(util::File::JoinPath(tmpdir, name), destination_path);
  util::File::SetSHA(store_directory_, hash, &out_info);
  *options->add_output() = std::move(out_info);
}

void LocalExecutor::MaybeRequestFile(const proto::FileInfo& info,
                                     const RequestFileCallback& file_callback) {
  std::string path = util::File::ProtoSHAToPath(store_directory_, info.hash());
  if (util::File::Size(path) >= 0) return;
  const bool overwrite = false;
  const bool exist_ok = false;
  if (info.has_contents()) {
    util::File::Write(path, info.contents(), overwrite, exist_ok);
  } else {
    using std::placeholders::_1;
    util::File::Write(path, std::bind(file_callback, info.hash(), _1),
                      overwrite, exist_ok);
  }
}

void LocalExecutor::GetFile(const proto::SHA256& hash,
                            const util::File::ChunkReceiver& chunk_receiver) {
  util::File::Read(util::File::ProtoSHAToPath(store_directory_, hash),
                   chunk_receiver);
}

LocalExecutor::LocalExecutor(std::string store_directory,
                             std::string temp_directory, size_t num_cores)
    : store_directory_(std::move(store_directory)),
      temp_directory_(std::move(temp_directory)) {
  util::File::MakeDirs(temp_directory_);
  util::File::MakeDirs(store_directory_);

  if (num_cores == 0) {
    num_cores = std::thread::hardware_concurrency();
  }
  ThreadGuard::SetMaxThreads(num_cores);
}

LocalExecutor::ThreadGuard::ThreadGuard(bool exclusive)
    : exclusive_(exclusive) {
  std::lock_guard<std::mutex> lck(Mutex());
  if (exclusive_) {
    if (CurThreads() != 0) {
      throw too_many_executions("Exclusive execution failed: worker busy");
    }
    CurThreads() = MaxThreads();
  } else {
    if (CurThreads() == MaxThreads()) {
      throw too_many_executions("Execution failed: worker busy");
    }
    CurThreads()++;
  }
}

LocalExecutor::ThreadGuard::~ThreadGuard() {
  std::lock_guard<std::mutex> lck(Mutex());
  CurThreads() = exclusive_ ? 0 : (CurThreads() - 1);
}

void LocalExecutor::ThreadGuard::SetMaxThreads(size_t num) {
  MaxThreads() = num;
}

size_t& LocalExecutor::ThreadGuard::MaxThreads() {
  static size_t max = 0;
  return max;
}

size_t& LocalExecutor::ThreadGuard::CurThreads() {
  static size_t cur = 0;
  return cur;
}

std::mutex& LocalExecutor::ThreadGuard::Mutex() {
  static std::mutex mtx;
  return mtx;
}
}  // namespace executor
/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/drive/tools/launcher.h"

#include <vector>

#include "boost/interprocess/sync/scoped_lock.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/process/execute.hpp"
#include "boost/process/initializers.hpp"
#include "boost/process/terminate.hpp"
#include "boost/process/wait_for_exit.hpp"

#include "maidsafe/common/crypto.h"
#include "maidsafe/common/error.h"
#include "maidsafe/common/ipc.h"
#include "maidsafe/common/on_scope_exit.h"
#include "maidsafe/common/process.h"
#include "maidsafe/common/utils.h"

namespace bi = boost::interprocess;
namespace bp = boost::process;
namespace bptime = boost::posix_time;
namespace fs = boost::filesystem;

namespace maidsafe {

namespace drive {

#ifdef MAIDSAFE_WIN32
boost::filesystem::path GetNextAvailableDrivePath() {
  uint32_t drive_letters(GetLogicalDrives()), mask(0x4);
  std::string path("C:");
  while (drive_letters & mask) {
    mask <<= 1;
    ++path[0];
  }
  if (path[0] > 'Z')
    ThrowError(DriveErrors::no_drive_letter_available);
  return boost::filesystem::path(path);
}

namespace {

fs::path AdjustMountPath(const fs::path& mount_path) {
  return mount_path / fs::path("/").make_preferred();
}

#else

namespace {

fs::path AdjustMountPath(const fs::path& mount_path) { return mount_path; }

#endif

enum SharedMemoryArgIndex {
  kMountPathArg = 0,
  kStoragePathArg,
  kUniqueIdArg,
  kRootParentIdArg,
  kDriveNameArg,
  kCreateStoreArg,
  kMaxArgIndex
};

void DoNotifyMountStatus(const std::string& mount_status_shared_object_name, bool mount_and_wait) {
  bi::shared_memory_object shared_object(bi::open_only, mount_status_shared_object_name.c_str(),
                                         bi::read_write);
  bi::mapped_region region(shared_object, bi::read_write);
  auto mount_status = static_cast<MountStatus*>(region.get_address());
  bi::scoped_lock<bi::interprocess_mutex> lock(mount_status->mutex);
  mount_status->mounted = mount_and_wait;
  mount_status->condition.notify_one();
  if (mount_and_wait)
    mount_status->condition.wait(lock, [&] { return mount_status->unmount; });
}

}  // unnamed namespace

std::string GetMountStatusSharedMemoryName(const std::string& initial_shared_memory_name) {
  return HexEncode(crypto::Hash<crypto::SHA512>(initial_shared_memory_name)).substr(0, 32);
}

void ReadAndRemoveInitialSharedMemory(const std::string& initial_shared_memory_name,
                                      Options& options) {
  auto shared_memory_args = ipc::ReadSharedMemory(initial_shared_memory_name.c_str(), kMaxArgIndex);
  options.mount_path = shared_memory_args[kMountPathArg];
  options.storage_path = shared_memory_args[kStoragePathArg];
  options.unique_id = maidsafe::Identity(shared_memory_args[kUniqueIdArg]);
  options.root_parent_id = maidsafe::Identity(shared_memory_args[kRootParentIdArg]);
  options.drive_name = shared_memory_args[kDriveNameArg];
  options.create_store = (std::stoi(shared_memory_args[kCreateStoreArg]) != 0);
  options.mount_status_shared_object_name =
      GetMountStatusSharedMemoryName(initial_shared_memory_name);
  ipc::RemoveSharedMemory(initial_shared_memory_name);
}

void NotifyMountedAndWaitForUnmountRequest(const std::string& mount_status_shared_object_name) {
  DoNotifyMountStatus(mount_status_shared_object_name, true);
}

void NotifyUnmounted(const std::string& mount_status_shared_object_name) {
  DoNotifyMountStatus(mount_status_shared_object_name, false);
}

Launcher::Launcher(const Options& options)
    : initial_shared_memory_name_(RandomAlphaNumericString(32)),
      kMountPath_(AdjustMountPath(options.mount_path)), mount_status_shared_object_(),
      mount_status_mapped_region_(), mount_status_(nullptr), drive_process_() {
  maidsafe::on_scope_exit cleanup_on_throw([&] {
    try {
      StopDriveProcess();
    }
    catch (const std::exception& e) {
      LOG(kError) << e.what();
    }
  });
  CreateInitialSharedMemory(options);
  CreateMountStatusSharedMemory();
  StartDriveProcess(options);
  WaitForDriveToMount();
  cleanup_on_throw.Release();
}

void Launcher::CreateInitialSharedMemory(const Options& options) {
  std::vector<std::string> shared_memory_args(kMaxArgIndex);
  shared_memory_args[kMountPathArg] = options.mount_path.string();
  shared_memory_args[kStoragePathArg] = options.storage_path.string();
  shared_memory_args[kUniqueIdArg] = options.unique_id.string();
  shared_memory_args[kRootParentIdArg] = options.root_parent_id.string();
  shared_memory_args[kDriveNameArg] = options.drive_name.string();
  shared_memory_args[kCreateStoreArg] = options.create_store ? "1" : "0";
  ipc::CreateSharedMemory(initial_shared_memory_name_, shared_memory_args);
}

void Launcher::CreateMountStatusSharedMemory() {
  mount_status_shared_object_ = bi::shared_memory_object(bi::create_only,
      GetMountStatusSharedMemoryName(initial_shared_memory_name_).c_str(), bi::read_write);
  mount_status_shared_object_.truncate(sizeof(MountStatus));
  mount_status_mapped_region_ = bi::mapped_region(mount_status_shared_object_, bi::read_write);
  mount_status_ = new(mount_status_mapped_region_.get_address()) MountStatus;
}

void Launcher::StartDriveProcess(const Options& options) {
  // Set up boost::process args
  std::vector<std::string> process_args;
  const auto kExePath(GetDriveExecutablePath(options.drive_type).string());
  process_args.push_back(kExePath);
  process_args.emplace_back("--shared_memory " + initial_shared_memory_name_);
  if (!options.drive_logging_args.empty())
    process_args.push_back(options.drive_logging_args);
  const auto kCommandLine(process::ConstructCommandLine(process_args));

  // Start drive process
  boost::system::error_code error_code;
  drive_process_.reset(new bp::child(bp::execute(bp::initializers::run_exe(kExePath),
                                                 bp::initializers::set_cmd_line(kCommandLine),
                                                 bp::initializers::set_on_error(error_code))));
  if (error_code) {
    LOG(kError) << "Failed to start local drive: " << error_code.message();
    ThrowError(CommonErrors::uninitialised);
  }
}

fs::path Launcher::GetDriveExecutablePath(DriveType drive_type) {
  switch (drive_type) {
    case DriveType::kLocal:
      return process::GetOtherExecutablePath("local_drive");
    case DriveType::kLocalConsole:
      return process::GetOtherExecutablePath("local_drive_console");
    case DriveType::kNetwork:
      return process::GetOtherExecutablePath("network_drive");
    case DriveType::kNetworkConsole:
      return process::GetOtherExecutablePath("network_drive_console");
    default:
      ThrowError(CommonErrors::invalid_parameter);
  }
  return "";
}

void Launcher::WaitForDriveToMount() {
  bi::scoped_lock<bi::interprocess_mutex> lock(mount_status_->mutex);
  auto timeout(bptime::second_clock::universal_time() + bptime::seconds(10));
  if (!mount_status_->condition.timed_wait(lock, timeout, [&] { return mount_status_->mounted; })) {
    LOG(kError) << "Failed waiting for drive to mount.";
    ThrowError(DriveErrors::failed_to_mount);
  }
}

Launcher::~Launcher() {
  try {
    StopDriveProcess();
  }
  catch (const std::exception& e) {
    LOG(kError) << e.what();
  }
  ipc::RemoveSharedMemory(initial_shared_memory_name_);
  bi::shared_memory_object::remove(
      GetMountStatusSharedMemoryName(initial_shared_memory_name_).c_str());
}

void Launcher::StopDriveProcess() {
  if (!drive_process_)
    return;
  bi::scoped_lock<bi::interprocess_mutex> lock(mount_status_->mutex);
  mount_status_->unmount = true;
  mount_status_->condition.notify_one();
  bptime::ptime timeout(bptime::second_clock::universal_time() + bptime::seconds(10));
  boost::system::error_code error_code;
  if (!mount_status_->condition.timed_wait(lock, timeout,
                                           [&] { return !mount_status_->mounted; })) {
    LOG(kError) << "Failed waiting for drive to unmount - terminating drive process.";
    bp::terminate(*drive_process_, error_code);
    drive_process_ = nullptr;
    return;
  }
  auto exit_code = bp::wait_for_exit(*drive_process_, error_code);
  if (error_code)
    LOG(kError) << "Error waiting for drive process to exit: " << error_code.message();
  else
    LOG(kInfo) << "Drive process has completed with exit code " << exit_code;
  drive_process_ = nullptr;
}

}  // namespace drive

}  // namespace maidsafe

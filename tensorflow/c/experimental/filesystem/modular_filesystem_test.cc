/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <memory>
#include <random>
#include <string>

#include "tensorflow/c/tf_status.h"
#include "tensorflow/c/tf_status_internal.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/error.h"
#include "tensorflow/core/platform/stacktrace_handler.h"
#include "tensorflow/core/platform/str_util.h"
#include "tensorflow/core/platform/strcat.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/util/command_line_flags.h"

// TODO(b/143949264): Testing is not yet supported on Windows. Will implement
// testing on Windows when implementing modular filesystems on Windows.
#if defined(PLATFORM_WINDOWS)
#error Windows is not yet supported.  Need mkdir().
#endif

// The tests defined here test the compliance of filesystems with the API
// defined by `filesystem_interface.h`.
//
// As some filesystems require special setup, these tests are run manually.
//
// Each filesystem implementation can be provided by DSOs, so we provide the
// `--dsos` flag to specify a list of shared objects to be loaded in order.
// If the flag is not used, no shared objects are loaded.
//
// Every filesystem provides support for accessing URIs of form
// `[<scheme>://]<path>` where `<scheme>` is optional (if missing, we are
// accessing local paths). This test suite tests exactly one scheme for each
// invocation. By default, we are testing all schemes available but this can be
// restricted by using `--schemes` to specify a set of schemes to test.
//
// Example invocation:
//  bazel test //tensorflow/c/experimental/filesystem:modular_filesystem_test \\
//  --test_arg=--dso=/path/to/one.so --test_arg=--dso=/path/to/another.so \\
//  --test_arg=--scheme= --test_arg=--scheme=file
//
// Note that to test the local filesystem we use an empty value.

namespace tensorflow {
namespace {

using ::tensorflow::error::Code;

// As we need to test multiple URI schemes we need a parameterized test.
// Furthermore, since each test creates and deletes files, we will use the same
// fixture to create new directories in `SetUp`. Each directory will reside in
// `::testing::TempDir()`, will use a RNG component and the test name. This
// ensures that two consecutive runs are unlikely to clash.
class ModularFileSystemTest : public ::testing::TestWithParam<std::string> {
 public:
  // Initializes `root_dir_` to a unique value made of `::testing::TempDir()`, a
  // static random value unique for all the tests in one invocation, and the
  // current test name.
  //
  // Since the test name contains `/` (due to parameters), this function
  // replaces `/` with `_`.
  //
  // We trade in one extra initialization for readability.
  ModularFileSystemTest() {
    const std::string test_name = tensorflow::str_util::StringReplace(
        ::testing::UnitTest::GetInstance()->current_test_info()->name(), "/",
        "_", /*replace_all=*/true);
    root_dir_ = tensorflow::io::JoinPath(
        ::testing::TempDir(),
        tensorflow::strings::StrCat("tf_fs_", rng_val_, "_", test_name));
    env_ = Env::Default();
  }

  void SetUp() override {
    // TODO(b/143949264): Testing is not yet supported on Windows. Will
    // implement testing on Windows when implementing modular filesystems on
    // Windows.
    if (mkdir(root_dir_.c_str(), 0755) != 0) {
      int error_code = errno;
      GTEST_SKIP() << "Cannot create working directory: "
                   << tensorflow::IOError(root_dir_, error_code);
    }
  }

  // Converts path reference to URI reference.
  //
  // If URI scheme is empty, URI reference is `path` relative to current test
  // root directory. Otherwise, we need to add the `<scheme>://` in front of
  // this path.
  //
  // TODO(mihaimaruseac): Note that some filesystem might require a different
  // approach here, for example they might require the root directory path to
  // be in a special format, etc. When we get there, we might decide to move
  // this class to `modular_filesystem_test.h` and extend the instantiation to
  // also take as argument an implementation for this method/a subclass factory
  // (see
  // https://github.com/google/googletest/blob/master/googletest/docs/advanced.md#creating-value-parameterized-abstract-tests)
  std::string GetURIForPath(StringPiece path) {
    const std::string translated_name =
        tensorflow::io::JoinPath(root_dir_, path);
    if (GetParam().empty()) return translated_name;

    return tensorflow::strings::StrCat(GetParam(), "://", translated_name);
  }

  // Converts absolute paths to paths relative to root_dir_.
  StringPiece GetRelativePath(StringPiece absolute_path) {
    return tensorflow::str_util::StripPrefix(absolute_path, root_dir_);
  }

  // Initializes the randomness used to ensure test isolation.
  static void InitializeTestRNG() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distribution;
    rng_val_ = distribution(gen);
  }

 protected:
  Env* env_;

 private:
  std::string root_dir_;
  static int rng_val_;
};

int ModularFileSystemTest::rng_val_;

// As some of the implementations might be missing, the tests should still pass
// if the returned `Status` signals the unimplemented state.
bool UninmplementedOrReturnsCode(Status actual_status, Code expected_code) {
  Code actual_code = actual_status.code();
  return (actual_code == Code::UNIMPLEMENTED) || (actual_code == expected_code);
}

TEST_P(ModularFileSystemTest, TestTranslateName) {
  const std::string generic_path = GetURIForPath("some_path");
  FileSystem* fs = nullptr;
  Status s = env_->GetFileSystemForFile(generic_path, &fs);
  if (fs == nullptr || !s.ok()) GTEST_SKIP() << "No filesystem registered";

  // First, test some interesting corner cases concerning empty URIs
  if (GetParam().empty()) {
    EXPECT_EQ(fs->TranslateName(""), "");
    EXPECT_EQ(fs->TranslateName("/"), "/");
    EXPECT_EQ(fs->TranslateName("//"), "/");
    // Empty scheme also allows relative paths
    EXPECT_EQ(fs->TranslateName("a_file"), "a_file");
    EXPECT_EQ(fs->TranslateName("a_dir/.."), ".");
  } else {
    EXPECT_EQ(fs->TranslateName(tensorflow::strings::StrCat(GetParam(), "://")),
              "/");
    EXPECT_EQ(
        fs->TranslateName(tensorflow::strings::StrCat(GetParam(), ":///")),
        "/");
    EXPECT_EQ(
        fs->TranslateName(tensorflow::strings::StrCat(GetParam(), ":////")),
        "/");
  }

  // Now test several paths/URIs
  EXPECT_EQ(GetRelativePath(fs->TranslateName(GetURIForPath("a_file"))),
            "/a_file");
  EXPECT_EQ(GetRelativePath(fs->TranslateName(GetURIForPath("a_dir/a_file"))),
            "/a_dir/a_file");
  EXPECT_EQ(GetRelativePath(fs->TranslateName(GetURIForPath("./a_file"))),
            "/a_file");
  EXPECT_EQ(GetRelativePath(fs->TranslateName(
                GetURIForPath("a/convoluted/../path/./to/.//.///a/file"))),
            "/a/path/to/a/file");
}

TEST_P(ModularFileSystemTest, TestCreateFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewWritableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::OK);
}

TEST_P(ModularFileSystemTest, TestCreateFileNonExisting) {
  const std::string filepath = GetURIForPath("dir_not_found/a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewWritableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::NOT_FOUND);
}

TEST_P(ModularFileSystemTest, TestCreateFileExistingDir) {
  const std::string filepath = GetURIForPath("a_file");
  Status status = env_->CreateDir(filepath);
  if (!status.ok()) GTEST_SKIP() << "CreateDir() not supported";

  std::unique_ptr<WritableFile> new_file;
  status = env_->NewWritableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestCreateFilePathIsInvalid) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> file;
  Status status = env_->NewWritableFile(filepath, &file);
  if (!status.ok()) GTEST_SKIP() << "CreateDir() not supported";

  const std::string new_path = GetURIForPath("a_file/a_file");
  std::unique_ptr<WritableFile> new_file;
  status = env_->NewWritableFile(new_path, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestAppendFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewAppendableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::OK);
}

TEST_P(ModularFileSystemTest, TestAppendFileNonExisting) {
  const std::string filepath = GetURIForPath("dir_not_found/a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewAppendableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::NOT_FOUND);
}

TEST_P(ModularFileSystemTest, TestAppendFileExistingDir) {
  const std::string filepath = GetURIForPath("a_file");
  Status status = env_->CreateDir(filepath);
  if (!status.ok()) GTEST_SKIP() << "CreateDir() not supported";

  std::unique_ptr<WritableFile> new_file;
  status = env_->NewAppendableFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestCreateThenAppendFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewWritableFile(filepath, &new_file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  std::unique_ptr<WritableFile> same_file;
  status = env_->NewAppendableFile(filepath, &same_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::OK);
}

TEST_P(ModularFileSystemTest, TestAppendFilePathIsInvalid) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> file;
  Status status = env_->NewWritableFile(filepath, &file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  const std::string new_path = GetURIForPath("a_file/a_file");
  std::unique_ptr<WritableFile> same_file;
  status = env_->NewAppendableFile(new_path, &same_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestReadFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<RandomAccessFile> new_file;
  Status status = env_->NewRandomAccessFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::NOT_FOUND);
}

TEST_P(ModularFileSystemTest, TestReadFileNonExisting) {
  const std::string filepath = GetURIForPath("dir_not_found/a_file");
  std::unique_ptr<RandomAccessFile> new_file;
  Status status = env_->NewRandomAccessFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::NOT_FOUND);
}

TEST_P(ModularFileSystemTest, TestReadFileExistingDir) {
  const std::string filepath = GetURIForPath("a_file");
  Status status = env_->CreateDir(filepath);
  if (!status.ok()) GTEST_SKIP() << "CreateDir() not supported";

  std::unique_ptr<RandomAccessFile> new_file;
  status = env_->NewRandomAccessFile(filepath, &new_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestCreateThenReadFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewWritableFile(filepath, &new_file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  std::unique_ptr<RandomAccessFile> same_file;
  status = env_->NewRandomAccessFile(filepath, &same_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::OK);
}

TEST_P(ModularFileSystemTest, TestReadFilePathIsInvalid) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> file;
  Status status = env_->NewWritableFile(filepath, &file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  const std::string new_path = GetURIForPath("a_file/a_file");
  std::unique_ptr<RandomAccessFile> same_file;
  status = env_->NewRandomAccessFile(new_path, &same_file);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

TEST_P(ModularFileSystemTest, TestCreateDir) {
  const std::string dirpath = GetURIForPath("a_dir");
  Status status = env_->CreateDir(dirpath);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::OK);
}

TEST_P(ModularFileSystemTest, TestCreateDirNoParent) {
  const std::string dirpath = GetURIForPath("dir_not_found/a_dir");
  Status status = env_->CreateDir(dirpath);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::NOT_FOUND);
}

TEST_P(ModularFileSystemTest, TestCreateDirWhichIsFile) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> new_file;
  Status status = env_->NewWritableFile(filepath, &new_file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  status = env_->CreateDir(filepath);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::ALREADY_EXISTS);
}

TEST_P(ModularFileSystemTest, TestCreateDirTwice) {
  const std::string dirpath = GetURIForPath("a_dir");
  Status status = env_->CreateDir(dirpath);
  if (!status.ok()) GTEST_SKIP() << "CreateDir() not supported";

  status = env_->CreateDir(dirpath);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::ALREADY_EXISTS);
}

TEST_P(ModularFileSystemTest, TestCreateDirPathIsInvalid) {
  const std::string filepath = GetURIForPath("a_file");
  std::unique_ptr<WritableFile> file;
  Status status = env_->NewWritableFile(filepath, &file);
  if (!status.ok()) GTEST_SKIP() << "NewWritableFile() not supported";

  const std::string new_path = GetURIForPath("a_file/a_dir");
  status = env_->CreateDir(new_path);
  EXPECT_PRED2(UninmplementedOrReturnsCode, status, Code::FAILED_PRECONDITION);
}

// The URI schemes that need to be tested are provided by the user via flags
// (or, if none is supplied, all existing schemes are used). As a scheme can
// become available after a shared object with a filesystem implementation is
// loaded, we can only check for availability after all arguments have been
// parsed.
//
// Furthermore, as `INSTANTIATE_TEST_SUITE_P` needs to be at global level and we
// don't want to have a `std::vector<std::string>` at global level, we use a
// static pointer to such a vector: we construct it via `SchemeVector()` below
// and when tests are instantiated we process it using `GetSchemes()`.
static std::vector<std::string>* SchemeVector() {
  static std::vector<std::string>* schemes = new std::vector<std::string>;
  return schemes;
}

static std::vector<std::string> GetSchemes() {
  std::vector<std::string>* user_schemes = SchemeVector();
  std::vector<std::string> all_schemes;
  tensorflow::Status status =
      tensorflow::Env::Default()->GetRegisteredFileSystemSchemes(&all_schemes);

  if (status.ok()) {
    if (!user_schemes->empty()) {
      auto is_registered_scheme = [&all_schemes](const auto& scheme) {
        return std::find(all_schemes.begin(), all_schemes.end(), scheme) ==
               all_schemes.end();
      };
      auto end = std::remove_if(user_schemes->begin(), user_schemes->end(),
                                is_registered_scheme);
      user_schemes->erase(end, user_schemes->end());
      return *user_schemes;
    }

    // Next, try all schemes available
    if (!all_schemes.empty()) return all_schemes;
  }

  // Fallback: no filesystems present, hence no tests
  return std::vector<std::string>();
}

INSTANTIATE_TEST_SUITE_P(ModularFileSystem, ModularFileSystemTest,
                         ::testing::ValuesIn(GetSchemes()));

// Loads a shared object implementing filesystem functionality.
static bool LoadDSO(const std::string& dso) {
  void* dso_handle;
  tensorflow::Status status =
      tensorflow::Env::Default()->LoadLibrary(dso.c_str(), &dso_handle);
  if (!status.ok()) {
    VLOG(0) << "Couldn't load DSO: " << status;
    return false;
  }

  void* dso_symbol;
  status = tensorflow::Env::Default()->GetSymbolFromLibrary(
      dso_handle, "TF_InitPlugin", &dso_symbol);
  if (!status.ok()) {
    VLOG(0) << "Couldn't load TF_InitPlugin: " << status;
    return false;
  }

  TF_Status* s = TF_NewStatus();
  (reinterpret_cast<void (*)(TF_Status*)>(dso_symbol))(s);
  if (!s->status.ok()) {
    VLOG(0) << "Couldn't initialize plugin: " << s->status;
    TF_DeleteStatus(s);
    return false;
  }
  TF_DeleteStatus(s);

  return true;
}

// Tests whether a URI scheme results in a filesystem that is supported.
//
// As we need these URI schemes to instantiate the test suite when
// `testing::InitGoogleTest` gets called, here we just store them to an
// internal scheme registry. See `URISchemeRegister` above.
static bool GetURIScheme(const std::string& scheme) {
  tensorflow::SchemeVector()->push_back(scheme);
  return true;
}

}  // namespace
}  // namespace tensorflow

// Due to the usages of flags for this manual test, we need a special `main` to
// ensure our flags are parsed properly as `testing::InitGoogleTest` silently
// ignores other flags. Furthermore, we need this to ensure that the DSO is
// loaded exactly once, if provided.
GTEST_API_ int main(int argc, char** argv) {
  const std::vector<tensorflow::Flag> flag_list = {
      tensorflow::Flag("dso", tensorflow::LoadDSO, "",
                       "Path to shared object to load"),
      tensorflow::Flag("scheme", tensorflow::GetURIScheme, "",
                       "URI scheme to test")};
  if (!tensorflow::Flags::Parse(&argc, argv, flag_list)) {
    std::cout << tensorflow::Flags::Usage(argv[0], flag_list);
    return -1;
  }

  tensorflow::testing::InstallStacktraceHandler();
  tensorflow::ModularFileSystemTest::InitializeTestRNG();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2014 Sandstorm Development Group, Inc. and contributors
// All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util.h"
#include <errno.h>
#include <kj/vector.h>
#include <kj/async-unix.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <syscall.h>
#include <signal.h>
#include <sys/wait.h>
#include <map>

namespace sandstorm {

Pipe Pipe::make() {
  int fds[2];
  KJ_SYSCALL(pipe2(fds, O_CLOEXEC));
  return { kj::AutoCloseFd(fds[0]), kj::AutoCloseFd(fds[1]) };
}

Pipe Pipe::makeAsync() {
  int fds[2];
  KJ_SYSCALL(pipe2(fds, O_CLOEXEC | O_ASYNC));
  return { kj::AutoCloseFd(fds[0]), kj::AutoCloseFd(fds[1]) };
}

kj::AutoCloseFd raiiOpen(kj::StringPtr name, int flags, mode_t mode) {
  int fd;
  KJ_SYSCALL(fd = open(name.cStr(), flags, mode), name);
  return kj::AutoCloseFd(fd);
}

kj::AutoCloseFd raiiOpenAt(int dirfd, kj::StringPtr name, int flags, mode_t mode) {
  int fd;
  if (flags & O_TMPFILE) {
    // work around glibc bug: https://sourceware.org/bugzilla/show_bug.cgi?id=17523
    KJ_SYSCALL(fd = syscall(SYS_openat, dirfd, name.cStr(), flags, mode), name);
  } else {
    KJ_SYSCALL(fd = openat(dirfd, name.cStr(), flags, mode), name);
  }
  return kj::AutoCloseFd(fd);
}

kj::Maybe<kj::AutoCloseFd> raiiOpenIfExists(kj::StringPtr name, int flags, mode_t mode) {
  int fd = open(name.cStr(), flags, mode);
  if (fd == -1) {
    if (errno == ENOENT) {
      return nullptr;
    } else {
      KJ_FAIL_SYSCALL("open", errno, name);
    }
  } else {
    return kj::AutoCloseFd(fd);
  }
}

kj::Maybe<kj::AutoCloseFd> raiiOpenAtIfExists(
    int dirfd, kj::StringPtr name, int flags, mode_t mode) {
  int fd = openat(dirfd, name.cStr(), flags, mode);
  if (fd == -1) {
    if (errno == ENOENT) {
      return nullptr;
    } else {
      KJ_FAIL_SYSCALL("open", errno, name);
    }
  } else {
    return kj::AutoCloseFd(fd);
  }
}

kj::Maybe<kj::String> readLine(kj::BufferedInputStream& input) {
  kj::Vector<char> result(80);

  for (;;) {
    auto buffer = input.tryGetReadBuffer();
    if (buffer.size() == 0) {
      KJ_REQUIRE(result.size() == 0, "Got partial line.");
      return nullptr;
    }
    for (size_t i: kj::indices(buffer)) {
      if (buffer[i] == '\n') {
        input.skip(i+1);
        result.add('\0');
        return kj::String(result.releaseAsArray());
      } else {
        result.add(buffer[i]);
      }
    }
    input.skip(buffer.size());
  }
}

kj::ArrayPtr<const char> trimArray(kj::ArrayPtr<const char> slice) {
  while (slice.size() > 0 && isspace(slice[0])) {
    slice = slice.slice(1, slice.size());
  }
  while (slice.size() > 0 && isspace(slice[slice.size() - 1])) {
    slice = slice.slice(0, slice.size() - 1);
  }

  return slice;
}

kj::String trim(kj::ArrayPtr<const char> slice) {
  return kj::heapString(trimArray(slice));
}

void toLower(kj::ArrayPtr<char> text) {
  for (char& c: text) {
    if ('A' <= c && c <= 'Z') {
      c = c - 'A' + 'a';
    }
  }
}

kj::Maybe<uint> parseUInt(kj::StringPtr s, int base) {
  char* end;
  uint result = strtoul(s.cStr(), &end, base);
  if (s.size() == 0 || *end != '\0') {
    return nullptr;
  }
  return result;
}

kj::AutoCloseFd openTemporary(kj::StringPtr near) {
  // TODO(someday):  Use O_TMPFILE?  New in Linux 3.11.

  int fd;
  auto name = kj::str(near, ".XXXXXX");
  KJ_SYSCALL(fd = mkostemp(name.begin(), O_CLOEXEC), name);
  kj::AutoCloseFd result(fd);
  KJ_SYSCALL(unlink(name.cStr()));
  return result;
}

bool isDirectory(kj::StringPtr path) {
  struct stat stats;
  KJ_SYSCALL(lstat(path.cStr(), &stats));
  return S_ISDIR(stats.st_mode);
}

static kj::Array<kj::String> listDirectoryAndClose(DIR* dir) {
  KJ_DEFER(closedir(dir));
  kj::Vector<kj::String> entries;

  for (;;) {
    errno = 0;
    struct dirent* entry = readdir(dir);
    if (entry == nullptr) {
      int error = errno;
      if (error == 0) {
        break;
      } else {
        KJ_FAIL_SYSCALL("readdir", error);
      }
    }

    kj::StringPtr name = entry->d_name;
    if (name != "." && name != "..") {
      entries.add(kj::heapString(entry->d_name));
    }
  }

  return entries.releaseAsArray();
}

kj::Array<kj::String> listDirectory(kj::StringPtr dirname) {
  DIR* dir = opendir(dirname.cStr());
  if (dir == nullptr) {
    KJ_FAIL_SYSCALL("opendir", errno, dirname);
  }
  return listDirectoryAndClose(dir);
}

kj::Array<kj::String> listDirectoryFd(int dirfd) {
  // We need to reopen the directory FD to get a separately-seekable file, and because closedir()
  // will close the fd even if opened with fdopendir().
  int fd;
  KJ_SYSCALL(fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY));
  DIR* dir = fdopendir(fd);
  if (dir == nullptr) {
    KJ_FAIL_SYSCALL("fdopendir", errno);
  }
  return listDirectoryAndClose(dir);
}

void recursivelyDelete(kj::StringPtr path) {
  struct stat stats;
  KJ_SYSCALL(lstat(path.cStr(), &stats), path) { return; }
  if (S_ISDIR(stats.st_mode)) {
    for (auto& file: listDirectory(path)) {
      recursivelyDelete(kj::str(path, "/", file));
    }
    KJ_SYSCALL(rmdir(path.cStr()), path) { break; }
  } else {
    KJ_SYSCALL(unlink(path.cStr()), path) { break; }
  }
}

void recursivelyCreateParent(kj::StringPtr path) {
  KJ_IF_MAYBE(pos, path.findLast('/')) {
    if (*pos == 0) return;

    kj::String parent = kj::heapString(path.slice(0, *pos));

    bool firstTry = true;
    while (mkdir(parent.cStr(), 0777) < 0) {
      int error = errno;
      if (firstTry && error == ENOENT) {
        recursivelyCreateParent(parent);
        firstTry = false;
      } else if (error == EEXIST) {
        break;
      } else if (error != EINTR) {
        KJ_FAIL_SYSCALL("mkdir(parent)", error, parent);
      }
    }
  }
}

kj::String readAll(int fd) {
  kj::FdInputStream input(fd);
  kj::Vector<char> content;
  for (;;) {
    char buffer[4096];
    size_t n = input.tryRead(buffer, sizeof(buffer), sizeof(buffer));
    content.addAll(buffer, buffer + n);
    if (n < sizeof(buffer)) {
      // Done!
      break;
    }
  }
  content.add('\0');
  return kj::String(content.releaseAsArray());
}

kj::String readAll(kj::StringPtr name) {
  return readAll(raiiOpen(name, O_RDONLY));
}

kj::Array<kj::String> splitLines(kj::String input) {
  size_t lineStart = 0;
  kj::Vector<kj::String> results;
  for (size_t i = 0; i < input.size(); i++) {
    if (input[i] == '\n' || input[i] == '#') {
      bool hasComment = input[i] == '#';
      input[i] = '\0';
      auto line = trim(input.slice(lineStart, i));
      if (line.size() > 0) {
        results.add(kj::mv(line));
      }
      if (hasComment) {
        // Ignore through newline.
        ++i;
        while (i < input.size() && input[i] != '\n') ++i;
      }
      lineStart = i + 1;
    }
  }

  if (lineStart < input.size()) {
    auto lastLine = trim(input.slice(lineStart));
    if (lastLine.size() > 0) {
      results.add(kj::mv(lastLine));
    }
  }

  return results.releaseAsArray();
}

kj::Vector<kj::ArrayPtr<const char>> split(kj::ArrayPtr<const char> input, char delim) {
  kj::Vector<kj::ArrayPtr<const char>> result;

  size_t start = 0;
  for (size_t i: kj::indices(input)) {
    if (input[i] == delim) {
      result.add(input.slice(start, i));
      start = i + 1;
    }
  }
  result.add(input.slice(start, input.size()));
  return result;
}

kj::Vector<kj::ArrayPtr<const char>> splitSpace(kj::ArrayPtr<const char> input) {
  kj::Vector<kj::ArrayPtr<const char>> result;

  size_t start = 0;
  for (size_t i: kj::indices(input)) {
    if (isspace(input[i])) {
      if (i > start) {
        result.add(input.slice(start, i));
      }
      start = i + 1;
    }
  }
  if (input.size() > start) {
    result.add(input.slice(start, input.size()));
  }
  return result;
}

kj::Maybe<kj::ArrayPtr<const char>> splitFirst(kj::ArrayPtr<const char>& input, char delim) {
  for (size_t i: kj::indices(input)) {
    if (input[i] == delim) {
      auto result = input.slice(0, i);
      input = input.slice(i + 1, input.size());
      return result;
    }
  }
  return nullptr;
}

kj::ArrayPtr<const char> extractHostFromUrl(kj::StringPtr url) {
  while (url.size() > 0 && 'a' <= url[0] && url[0] <= 'z') {
    url = url.slice(1);
  }
  KJ_REQUIRE(url.startsWith("://"), "Base URL does not have a protocol scheme?");
  url = url.slice(3);
  KJ_IF_MAYBE(slashPos, url.findFirst('/')) {
    return url.slice(0, *slashPos);
  } else {
    return url;
  }
}

kj::ArrayPtr<const char> extractProtocolFromUrl(kj::StringPtr url) {
  KJ_IF_MAYBE(colonPos, url.findFirst(':')) {
    return url.slice(0, *colonPos);
  } else {
    KJ_FAIL_REQUIRE("Base URL does not have a protocol scheme.", url);
  }
}

// =======================================================================================
// This code is derived from libb64 which has been placed in the public domain.
// For details, see http://sourceforge.net/projects/libb64

// -------------------------------------------------------------------
// Encoder

namespace {

typedef enum {
  step_A, step_B, step_C
} base64_encodestep;

typedef struct {
  base64_encodestep step;
  char result;
  int stepcount;
} base64_encodestate;

const int CHARS_PER_LINE = 72;

void base64_init_encodestate(base64_encodestate* state_in) {
  state_in->step = step_A;
  state_in->result = 0;
  state_in->stepcount = 0;
}

char base64_encode_value(char value_in) {
  static const char* encoding = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (value_in > 63) return '=';
  return encoding[(int)value_in];
}

int base64_encode_block(const char* plaintext_in, int length_in,
                        char* code_out, base64_encodestate* state_in, bool breakLines) {
  const char* plainchar = plaintext_in;
  const char* const plaintextend = plaintext_in + length_in;
  char* codechar = code_out;
  char result;
  char fragment;

  result = state_in->result;

  switch (state_in->step) {
    while (1) {
  case step_A:
      if (plainchar == plaintextend) {
        state_in->result = result;
        state_in->step = step_A;
        return codechar - code_out;
      }
      fragment = *plainchar++;
      result = (fragment & 0x0fc) >> 2;
      *codechar++ = base64_encode_value(result);
      result = (fragment & 0x003) << 4;
  case step_B:
      if (plainchar == plaintextend) {
        state_in->result = result;
        state_in->step = step_B;
        return codechar - code_out;
      }
      fragment = *plainchar++;
      result |= (fragment & 0x0f0) >> 4;
      *codechar++ = base64_encode_value(result);
      result = (fragment & 0x00f) << 2;
  case step_C:
      if (plainchar == plaintextend) {
        state_in->result = result;
        state_in->step = step_C;
        return codechar - code_out;
      }
      fragment = *plainchar++;
      result |= (fragment & 0x0c0) >> 6;
      *codechar++ = base64_encode_value(result);
      result  = (fragment & 0x03f) >> 0;
      *codechar++ = base64_encode_value(result);

      ++(state_in->stepcount);
      if (breakLines && state_in->stepcount == CHARS_PER_LINE/4) {
        *codechar++ = '\n';
        state_in->stepcount = 0;
      }
    }
  }
  /* control should not reach here */
  return codechar - code_out;
}

int base64_encode_blockend(char* code_out, base64_encodestate* state_in, bool breakLines) {
  char* codechar = code_out;

  switch (state_in->step) {
  case step_B:
    *codechar++ = base64_encode_value(state_in->result);
    *codechar++ = '=';
    *codechar++ = '=';
    ++state_in->stepcount;
    break;
  case step_C:
    *codechar++ = base64_encode_value(state_in->result);
    *codechar++ = '=';
    ++state_in->stepcount;
    break;
  case step_A:
    break;
  }
  if (breakLines && state_in->stepcount > 0) {
    *codechar++ = '\n';
  }

  return codechar - code_out;
}

}  // namespace

kj::String base64Encode(kj::ArrayPtr<const byte> input, bool breakLines) {
  /* set up a destination buffer large enough to hold the encoded data */
  // equivalent to ceil(input.size() / 3) * 4
  auto numChars = (input.size() + 2) / 3 * 4;
  if (breakLines) {
    // Add space for newline characters.
    uint lineCount = numChars / CHARS_PER_LINE;
    if (numChars % CHARS_PER_LINE > 0) {
      // Partial line.
      ++lineCount;
    }
    numChars = numChars + lineCount;
  }
  auto output = kj::heapString(numChars);
  /* keep track of our encoded position */
  char* c = output.begin();
  /* store the number of bytes encoded by a single call */
  int cnt = 0;
  size_t total = 0;
  /* we need an encoder state */
  base64_encodestate s;

  /*---------- START ENCODING ----------*/
  /* initialise the encoder state */
  base64_init_encodestate(&s);
  /* gather data from the input and send it to the output */
  cnt = base64_encode_block((const char *)input.begin(), input.size(), c, &s, breakLines);
  c += cnt;
  total += cnt;

  /* since we have encoded the entire input string, we know that
     there is no more input data; finalise the encoding */
  cnt = base64_encode_blockend(c, &s, breakLines);
  c += cnt;
  total += cnt;
  /*---------- STOP ENCODING  ----------*/

  KJ_ASSERT(total == output.size(), total, output.size());

  return output;
}

// -------------------------------------------------------------------
// Decoder

namespace {

typedef enum {
  step_a, step_b, step_c, step_d
} base64_decodestep;

typedef struct {
  base64_decodestep step;
  char plainchar;
} base64_decodestate;

int base64_decode_value(char value_in) {
  static const char decoding[] = {
    62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,-1,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,-1,
    26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};
  static const char decoding_size = sizeof(decoding);
  value_in -= 43;
  if (value_in < 0 || value_in > decoding_size) return -1;
  return decoding[(int)value_in];
}

void base64_init_decodestate(base64_decodestate* state_in) {
  state_in->step = step_a;
  state_in->plainchar = 0;
}

int base64_decode_block(const char* code_in, const int length_in,
                        char* plaintext_out, base64_decodestate* state_in) {
  const char* codechar = code_in;
  char* plainchar = plaintext_out;
  char fragment;

  *plainchar = state_in->plainchar;

  switch (state_in->step)
  {
    while (1)
    {
  case step_a:
      do {
        if (codechar == code_in+length_in) {
          state_in->step = step_a;
          state_in->plainchar = *plainchar;
          return plainchar - plaintext_out;
        }
        fragment = (char)base64_decode_value(*codechar++);
      } while (fragment < 0);
      *plainchar    = (fragment & 0x03f) << 2;
  case step_b:
      do {
        if (codechar == code_in+length_in) {
          state_in->step = step_b;
          state_in->plainchar = *plainchar;
          return plainchar - plaintext_out;
        }
        fragment = (char)base64_decode_value(*codechar++);
      } while (fragment < 0);
      *plainchar++ |= (fragment & 0x030) >> 4;
      *plainchar    = (fragment & 0x00f) << 4;
  case step_c:
      do {
        if (codechar == code_in+length_in) {
          state_in->step = step_c;
          state_in->plainchar = *plainchar;
          return plainchar - plaintext_out;
        }
        fragment = (char)base64_decode_value(*codechar++);
      } while (fragment < 0);
      *plainchar++ |= (fragment & 0x03c) >> 2;
      *plainchar    = (fragment & 0x003) << 6;
  case step_d:
      do {
        if (codechar == code_in+length_in) {
          state_in->step = step_d;
          state_in->plainchar = *plainchar;
          return plainchar - plaintext_out;
        }
        fragment = (char)base64_decode_value(*codechar++);
      } while (fragment < 0);
      *plainchar++   |= (fragment & 0x03f);
    }
  }
  /* control should not reach here */
  return plainchar - plaintext_out;
}

}  // namespace

kj::Array<byte> base64Decode(kj::StringPtr input) {
  base64_decodestate state;
  base64_init_decodestate(&state);

  auto output = kj::heapArray<byte>((input.size() * 6 + 7) / 8);

  size_t n = base64_decode_block(input.begin(), input.size(),
      reinterpret_cast<char*>(output.begin()), &state);

  if (n < output.size()) {
    auto copy = kj::heapArray<byte>(n);
    memcpy(copy.begin(), output.begin(), n);
    output = kj::mv(copy);
  }

  return output;
}

// =======================================================================================

kj::String hexEncode(kj::ArrayPtr<const byte> input) {
  const char DIGITS[] = "0123456789abcdef";
  return kj::strArray(KJ_MAP(b, input) { return kj::heapArray<char>({DIGITS[b/16], DIGITS[b%16]}); }, "");
}

Subprocess::Subprocess(Options&& options)
    : name(kj::heapString(options.argv.size() > 0 ? options.argv[0] : options.executable)) {
  KJ_SYSCALL(pid = fork());
  if (pid == 0) {
    KJ_DEFER(_exit(1));  // Do not under any circumstances return from this stack frame!
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      // Reset all signal handlers to default.  (exec() will leave ignored signals ignored, and KJ
      // code likes to ignore e.g. SIGPIPE.)
      // TODO(cleanup):  Is there a better way to do this?
      for (uint i = 0; i < NSIG; i++) {
        ::signal(i, SIG_DFL);  // Only possible error is EINVAL (invalid signum); we don't care.
      }

      // Unblock all signals.  (Yes, the signal mask is inherited over exec...)
      sigset_t sigmask;
      sigemptyset(&sigmask);
      KJ_SYSCALL(sigprocmask(SIG_SETMASK, &sigmask, nullptr));

      // Make sure all of the incoming FDs are outside of our map range (except for standard I/O if
      // it is already exactly in the right slot).
      int minFd = STDERR_FILENO + options.moreFds.size() + 1;

      if (options.stdin != STDIN_FILENO) forceFdAbove(options.stdin, minFd);
      if (options.stdout != STDOUT_FILENO) forceFdAbove(options.stdout, minFd);
      if (options.stderr != STDERR_FILENO) forceFdAbove(options.stderr, minFd);

      // Now remap.
      for (auto& fd: options.moreFds) {
        forceFdAbove(fd, minFd);
      }

      if (options.stdin != STDIN_FILENO) {
        KJ_SYSCALL(dup2(options.stdin, STDIN_FILENO));
      }
      if (options.stdout != STDOUT_FILENO) {
        KJ_SYSCALL(dup2(options.stdout, STDOUT_FILENO));
      }
      if (options.stderr != STDERR_FILENO) {
        KJ_SYSCALL(dup2(options.stderr, STDERR_FILENO));
      }

      for (auto i: kj::indices(options.moreFds)) {
        KJ_SYSCALL(dup2(options.moreFds[i], STDERR_FILENO + 1 + i));
      }

      // Make the args vector.
      char* argv[options.argv.size() + 1];
      for (auto i: kj::indices(options.argv)) {
        // exec*() is not const-correct. :(
        argv[i] = const_cast<char*>(options.argv[i].cStr());
      }
      argv[options.argv.size()] = nullptr;
      char** argvp = argv;  // lambda can't capture variable-size array

      KJ_IF_MAYBE(e, options.environment) {
        // Make the environment vector.
        char* environ[e->size() + 1];
        for (auto i: kj::indices(*e)) {
          // exec*() is not const-correct. :(
          environ[i] = const_cast<char*>((*e)[i].cStr());
        }
        environ[e->size()] = nullptr;
        char** environp = environ;  // lambda can't capture variable-size array

        if (options.searchPath) {
          KJ_SYSCALL(execvpe(options.executable.cStr(), argvp, environp), options.executable);
        } else {
          KJ_SYSCALL(execve(options.executable.cStr(), argvp, environp), options.executable);
        }
      } else {
        if (options.searchPath) {
          KJ_SYSCALL(execvp(options.executable.cStr(), argvp), options.executable);
        } else {
          KJ_SYSCALL(execv(options.executable.cStr(), argvp), options.executable);
        }
      }

      KJ_UNREACHABLE;
    })) {
      KJ_LOG(FATAL, *exception);
    }
  }
}

Subprocess::Subprocess(kj::Function<int()> func) {
  KJ_SYSCALL(pid = fork());
  if (pid == 0) {
    KJ_DEFER(_exit(1));  // Do not under any circumstances return from this stack frame!

    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      _exit(func());
    })) {
      KJ_LOG(FATAL, *exception);
    }
  }
}

Subprocess::~Subprocess() noexcept(false) {
  if (pid != 0) {
    unwindDetector.catchExceptionsIfUnwinding([this]() {
      signal(SIGKILL);
      (void)waitForExitOrSignal();
    });
  }
}

void Subprocess::signal(int signo) {
  if (pid != 0) {
    KJ_SYSCALL(kill(pid, signo), name);
  }
}

void Subprocess::waitForSuccess() {
  int exitCode = waitForExit();
  KJ_ASSERT(exitCode == 0, "child process failed", name, exitCode);
}

int Subprocess::waitForExit() {
  int status = waitForExitOrSignal();
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    int signo = WTERMSIG(status);
    KJ_FAIL_ASSERT("child process killed by signal", name, signo, strsignal(signo));
  } else {
    KJ_FAIL_ASSERT("unknown child wait status", name, status);
  }
}

int Subprocess::waitForExitOrSignal() {
  KJ_REQUIRE(pid != 0, "already waited for this child");
  int status;
  KJ_SYSCALL(waitpid(pid, &status, 0));
  pid = 0;
  return status;
}

void Subprocess::forceFdAbove(int& fd, int minValue) {
  // Force `fd` to have a numeric value of at least `minValue`.

  if (fd < minValue) {
    // We'll need to move this FD to a different slot. fcntl()'s F_DUPFD searches for a slot
    // greater than or equal to some value, which is exactly what we need! We want to set
    // O_CLOEXEC on this new FD because it is NOT the FD that we plan to keep in the child process;
    // we still plan to dup2() it back to the right slot.
    KJ_SYSCALL(fd = fcntl(fd, F_DUPFD_CLOEXEC, minValue));
  }
}

// -----------------------------------------------------------------------------

struct SubprocessSet::WaitMap {
  struct ProcInfo {
    kj::Own<kj::PromiseFulfiller<int>> fulfiller;
    Subprocess* subprocess;
  };

  std::map<pid_t, ProcInfo> pids;
};

SubprocessSet::SubprocessSet(kj::UnixEventPort& eventPort)
    : eventPort(eventPort), waitMap(kj::heap<WaitMap>()),
      waitTask(waitLoop().eagerlyEvaluate([](kj::Exception&& exception) {
        KJ_LOG(ERROR, "subprocess wait loop failed", exception);
      })) {
  kj::UnixEventPort::captureSignal(SIGCHLD);
}

SubprocessSet::~SubprocessSet() noexcept(false) {}

kj::Promise<void> SubprocessSet::waitForSuccess(Subprocess& subprocess) {
  return waitForExit(subprocess).then([&subprocess](int exitCode) {
    KJ_ASSERT(exitCode == 0, "child process failed", subprocess.name, exitCode);
  });
}

kj::Promise<int> SubprocessSet::waitForExit(Subprocess& subprocess) {
  return waitForExitOrSignal(subprocess).then([&subprocess](int status) {
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      int signo = WTERMSIG(status);
      KJ_FAIL_ASSERT("child process killed by signal", subprocess.name, signo, strsignal(signo));
    } else {
      KJ_FAIL_ASSERT("unknown child wait status", subprocess.name, status);
    }
  });
}

kj::Promise<int> SubprocessSet::waitForExitOrSignal(Subprocess& subprocess) {
  auto paf = kj::newPromiseAndFulfiller<int>();
  waitMap->pids.insert(std::make_pair(subprocess.getPid(),
      WaitMap::ProcInfo { kj::mv(paf.fulfiller), &subprocess }));
  return paf.promise.then([&subprocess](int status) {
    return status;
  });
}

kj::Promise<void> SubprocessSet::waitForSuccess(Subprocess&& subprocess) {
  auto heap = kj::heap<Subprocess>(kj::mv(subprocess));
  auto promise = waitForSuccess(*heap);
  return promise.attach(kj::mv(heap));
}
kj::Promise<int> SubprocessSet::waitForExit(Subprocess&& subprocess) {
  auto heap = kj::heap<Subprocess>(kj::mv(subprocess));
  auto promise = waitForExit(*heap);
  return promise.attach(kj::mv(heap));
}
kj::Promise<int> SubprocessSet::waitForExitOrSignal(Subprocess&& subprocess) {
  auto heap = kj::heap<Subprocess>(kj::mv(subprocess));
  auto promise = waitForExitOrSignal(*heap);
  return promise.attach(kj::mv(heap));
}

kj::Promise<void> SubprocessSet::waitLoop() {
  return eventPort.onSignal(SIGCHLD).then([this](auto&&) {
    while (!waitMap->pids.empty()) {
      int status;
      pid_t pid;
      KJ_SYSCALL(pid = waitpid(-1, &status, WNOHANG));
      if (pid == 0) break;

      auto iter = waitMap->pids.find(pid);
      if (iter == waitMap->pids.end()) {
        KJ_LOG(ERROR, "waitpid() returned unexpected PID; is this process running subprocesses "
                      "outside this set?", pid);
      } else {
        iter->second.subprocess->notifyExited(status);
        iter->second.fulfiller->fulfill(kj::mv(status));
        waitMap->pids.erase(iter);
      }
    }
    return waitLoop();
  });
}

}  // namespace sandstorm

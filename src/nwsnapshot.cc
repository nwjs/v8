// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <errno.h>
#include <stdio.h>
#include <signal.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <string>

#include "v8.h"

#include "bootstrapper.h"
#include "flags.h"
#include "natives.h"
#include "platform.h"
#include "serialize.h"
#include "snapshot.h"
#include "list.h"

using namespace v8;

static const unsigned int kMaxCounters = 256;

// A single counter in a counter collection.
class Counter {
 public:
  static const int kMaxNameSize = 64;
  int32_t* Bind(const char* name) {
    int i;
    for (i = 0; i < kMaxNameSize - 1 && name[i]; i++) {
      name_[i] = name[i];
    }
    name_[i] = '\0';
    return &counter_;
  }
 private:
  int32_t counter_;
  uint8_t name_[kMaxNameSize];
};


// A set of counters and associated information.  An instance of this
// class is stored directly in the memory-mapped counters file if
// the --save-counters options is used
class CounterCollection {
 public:
  CounterCollection() {
    magic_number_ = 0xDEADFACE;
    max_counters_ = kMaxCounters;
    max_name_size_ = Counter::kMaxNameSize;
    counters_in_use_ = 0;
  }
  Counter* GetNextCounter() {
    if (counters_in_use_ == kMaxCounters) return NULL;
    return &counters_[counters_in_use_++];
  }
 private:
  uint32_t magic_number_;
  uint32_t max_counters_;
  uint32_t max_name_size_;
  uint32_t counters_in_use_;
  Counter counters_[kMaxCounters];
};


class Compressor {
 public:
  virtual ~Compressor() {}
  virtual bool Compress(i::Vector<char> input) = 0;
  virtual i::Vector<char>* output() = 0;
};


class PartialSnapshotSink : public i::SnapshotByteSink {
 public:
  PartialSnapshotSink() : data_(), raw_size_(-1) { }
  virtual ~PartialSnapshotSink() { data_.Free(); }
  virtual void Put(int byte, const char* description) {
    data_.Add(byte);
  }
  virtual int Position() { return data_.length(); }
  void Print(FILE* fp) {
    int length = Position();
    for (int j = 0; j < length; j++) {
      if ((j & 0x1f) == 0x1f) {
        fprintf(fp, "\n");
      }
      if (j != 0) {
        fprintf(fp, ",");
      }
      fprintf(fp, "%u", static_cast<unsigned char>(at(j)));
    }
  }
  char at(int i) { return data_[i]; }
  bool Compress(Compressor* compressor) {
    ASSERT_EQ(-1, raw_size_);
    raw_size_ = data_.length();
    if (!compressor->Compress(data_.ToVector())) return false;
    data_.Clear();
    data_.AddAll(*compressor->output());
    return true;
  }
  int raw_size() { return raw_size_; }

 private:
  i::List<char> data_;
  int raw_size_;
};

class FileByteSink : public i::SnapshotByteSink {
 public:
  explicit FileByteSink(const char* snapshot_file) {
    memset(&header_, 0, sizeof(header_));
    fp_ = fopen(snapshot_file, "wb");
    file_name_ = snapshot_file;
    if (fp_ == NULL) {
      i::PrintF("Unable to write to snapshot file \"%s\"\n", snapshot_file);
      exit(1);
    }
    fwrite(&header_, sizeof(header_), 1, fp_);
  }
  virtual ~FileByteSink() {
    if (fp_ != NULL) {
      fclose(fp_);
    }
  }
  virtual void Put(int byte, const char* description) {
    if (fp_ != NULL) {
      fputc(byte, fp_);
    }
  }
  virtual int Position() {
    return ftell(fp_);
  }
  void WriteSpaceUsed(
      int new_space_used,
      int pointer_space_used,
      int data_space_used,
      int code_space_used,
      int map_space_used,
      int cell_space_used,
      int property_cell_space_used_);

 private:
  i::NWSnapshotHeader header_;
  FILE* fp_;
  const char* file_name_;
};


void FileByteSink::WriteSpaceUsed(
      int new_space_used,
      int pointer_space_used,
      int data_space_used,
      int code_space_used,
      int map_space_used,
      int cell_space_used,
      int property_space_used) {
  int size = ftell(fp_) - sizeof(header_);
  fseek(fp_, 0, SEEK_SET);

  header_.size                = size;
  header_.magic               = 11801102;
  header_.new_space_used      = new_space_used;
  header_.pointer_space_used  = pointer_space_used;
  header_.data_space_used     = data_space_used;
  header_.code_space_used     = code_space_used;
  header_.map_space_used      = map_space_used;
  header_.cell_space_used     = cell_space_used;
  header_.property_space_used = property_space_used;

  fwrite(&header_, sizeof(header_), 1, fp_);
}

int main(int argc, char** argv) {
  V8::InitializeICU();

  // By default, log code create information in the snapshot.
  i::FLAG_log_code = true;

  // Print the usage if an error occurs when parsing the command line
  // flags or if the help flag is set.
  int result = i::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (result > 0 || argc != 2 || i::FLAG_help) {
    ::printf("Usage: %s [flag] ... outfile\n", argv[0]);
    i::FlagList::PrintHelp();
    return !i::FLAG_help;
  }
  Isolate* isolate = v8::Isolate::New();
  isolate->Enter();
  i::Isolate* internal_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Serializer::Enable(internal_isolate);

  Persistent<Context> context;
  {
    HandleScope handle_scope(isolate);
    context.Reset(isolate, Context::New(isolate));
  }
  if (context.IsEmpty()) {
    fprintf(stderr,
            "\nException thrown while compiling natives - see above.\n\n");
    exit(1);
  }
  if (i::FLAG_extra_code != NULL) {
    // Capture 100 frames if anything happens.
    V8::SetCaptureStackTraceForUncaughtExceptions(true, 100);
    HandleScope scope(isolate);
    v8::Context::Scope scope2(v8::Local<v8::Context>::New(isolate, context));
    const char* name = i::FLAG_extra_code;
    FILE* file = i::OS::FOpen(name, "rb");
    if (file == NULL) {
      fprintf(stderr, "Failed to open '%s': errno %d\n", name, errno);
      exit(1);
    }

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    rewind(file);

    char* chars = new char[size + 1];
    chars[size] = '\0';
    for (int i = 0; i < size;) {
      int read = static_cast<int>(fread(&chars[i], 1, size - i, file));
      if (read < 0) {
        fprintf(stderr, "Failed to read '%s': errno %d\n", name, errno);
        exit(1);
      }
      i += read;
    }
    fclose(file);
    Local<String> source = String::New(chars);
    Local<String> filename = String::New(argv[1]);
    TryCatch try_catch;
    Local<Script> script = Script::New(source, filename, false);
    if (try_catch.HasCaught()) {
      fprintf(stderr, "Failure compiling '%s' (see above)\n", name);
      exit(1);
    }
    script->Run();
    if (try_catch.HasCaught()) {
      fprintf(stderr, "Failure running '%s'\n", name);
      Local<Message> message = try_catch.Message();
      Local<String> message_string = message->Get();
      Local<String> message_line = message->GetSourceLine();
      int len = 2 + message_string->Utf8Length() + message_line->Utf8Length();
      char* buf = new char(len);
      message_string->WriteUtf8(buf);
      fprintf(stderr, "%s at line %d\n", buf, message->GetLineNumber());
      message_line->WriteUtf8(buf);
      fprintf(stderr, "%s\n", buf);
      int from = message->GetStartColumn();
      int to = message->GetEndColumn();
      int i;
      for (i = 0; i < from; i++) fprintf(stderr, " ");
      for ( ; i <= to; i++) fprintf(stderr, "^");
      fprintf(stderr, "\n");
      exit(1);
    }
    i::Isolate* isolate = i::Isolate::Current();

#if 1
    {
      i::HandleScope scope(isolate);
      i::Handle<i::SharedFunctionInfo> function_info;
      i::Handle<i::Object> obj = v8::Utils::OpenHandle(*script);
      if (obj->IsSharedFunctionInfo()) {
        function_info =
          i::Handle<i::SharedFunctionInfo>(i::SharedFunctionInfo::cast(*obj));
        //i::SharedFunctionInfo::CompileLazy(function_info, i::KEEP_EXCEPTION);
      } else {
        function_info =
          i::Handle<i::SharedFunctionInfo>(i::JSFunction::cast(*obj)->shared());
        //i::SharedFunctionInfo::CompileLazy(function_info, i::KEEP_EXCEPTION);
      }
      i::Handle<i::Script> iscript(i::Script::cast(function_info->script()));
      iscript->set_source(isolate->heap()->undefined_value());
    }
#endif
  }
  // Make sure all builtin scripts are cached.
  { HandleScope scope;
    for (int i = 0; i < i::Natives::GetBuiltinsCount(); i++) {
      i::Isolate::Current()->bootstrapper()->NativesSourceLookup(i);
    }
  }
  // If we don't do this then we end up with a stray root pointing at the
  // context even after we have disposed of the context.
  internal_isolate->heap()->CollectAllGarbage(
      i::Heap::kNoGCFlags, "mksnapshot");
  i::Object* raw_context = *v8::Utils::OpenPersistent(context);
  context.Dispose();

  std::string partial_file(argv[1]);
  partial_file += ".p";

  bool failed = true;
  {
    FileByteSink startup_sink(argv[1]);
    i::StartupSerializer startup_serializer(internal_isolate, &startup_sink);
    startup_serializer.SerializeStrongReferences();

    FileByteSink partial_sink(partial_file.c_str());
    i::PartialSerializer p_ser(internal_isolate, &startup_serializer, &partial_sink);
    p_ser.Serialize(&raw_context);
    startup_serializer.SerializeWeakReferences();

    // the code above could fail in debug version

    failed = false;

    partial_sink.WriteSpaceUsed(
                                p_ser.CurrentAllocationAddress(i::NEW_SPACE),
                                p_ser.CurrentAllocationAddress(i::OLD_POINTER_SPACE),
                                p_ser.CurrentAllocationAddress(i::OLD_DATA_SPACE),
                                p_ser.CurrentAllocationAddress(i::CODE_SPACE),
                                p_ser.CurrentAllocationAddress(i::MAP_SPACE),
                                p_ser.CurrentAllocationAddress(i::CELL_SPACE),
                                p_ser.CurrentAllocationAddress(i::PROPERTY_CELL_SPACE));

    fprintf(stderr, "partial snapshot spaces: %d %d %d %d %d %d\n",
                                p_ser.CurrentAllocationAddress(i::NEW_SPACE),
                                p_ser.CurrentAllocationAddress(i::OLD_POINTER_SPACE),
                                p_ser.CurrentAllocationAddress(i::OLD_DATA_SPACE),
                                p_ser.CurrentAllocationAddress(i::CODE_SPACE),
                                p_ser.CurrentAllocationAddress(i::MAP_SPACE),
                                p_ser.CurrentAllocationAddress(i::CELL_SPACE));


    startup_sink.WriteSpaceUsed(
                                startup_serializer.CurrentAllocationAddress(i::NEW_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::OLD_POINTER_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::OLD_DATA_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::CODE_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::MAP_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::CELL_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::PROPERTY_CELL_SPACE));
    fprintf(stderr, "startup snapshot spaces: %d %d %d %d %d %d\n",
                                startup_serializer.CurrentAllocationAddress(i::NEW_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::OLD_POINTER_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::OLD_DATA_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::CODE_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::MAP_SPACE),
                                startup_serializer.CurrentAllocationAddress(i::CELL_SPACE));

    for (int idx = i::OLD_POINTER_SPACE; idx <= i::LAST_PAGED_SPACE; idx++) {
      if (internal_isolate->heap()->paged_space(idx)->AreaSize() <
          p_ser.CurrentAllocationAddress(idx)) {
        fprintf(stderr, "Error: Allocation in space %d is %d: bigger than %d\n",
                idx, p_ser.CurrentAllocationAddress(idx),
                internal_isolate->heap()->paged_space(idx)->AreaSize());
        failed = true;
      }
    }
    for (int idx = i::OLD_POINTER_SPACE; idx <= i::LAST_PAGED_SPACE; idx++) {
      if (internal_isolate->heap()->paged_space(idx)->AreaSize() <
          startup_serializer.CurrentAllocationAddress(idx)) {
        fprintf(stderr, "Error: Allocation in space %d is %d: bigger than %d\n",
                idx, startup_serializer.CurrentAllocationAddress(idx),
                internal_isolate->heap()->paged_space(idx)->AreaSize());
        failed = true;
      }
    }
  }

#ifdef _MSC_VER
#define unlink _unlink
#endif

  if (failed) {
    unlink(partial_file.c_str());
    unlink(argv[1]);
    exit(1);
  }

  fprintf(stderr, "Compiled successfully.\n");

  char buf[1024];
  FILE* fp = fopen(argv[1], "rb+");
  fseek(fp, 0, SEEK_END);
  FILE* fpp = fopen(partial_file.c_str(), "rb");
  size_t nread = 0;
  while (nread = fread(buf, 1, 1024, fpp)) {
    fwrite(buf, nread, 1, fp);
  }
  fclose(fpp);
  fclose(fp);
  unlink(partial_file.c_str());

  return 0;
}

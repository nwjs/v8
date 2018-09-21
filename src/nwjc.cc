// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Copyright 2013-2017 Intel Corp. Author: Roger Wang <roger.wang@intel.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wundefined-inline"
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include "src/v8.h"

#include "include/libplatform/libplatform.h"
#include "src/assembler.h"
#include "src/base/platform/platform.h"
#include "src/compiler.h"
#include "src/heap/factory.h"
#include "src/isolate-inl.h"
#include "src/flags.h"
#include "src/snapshot/natives.h"
#include "src/snapshot/code-serializer.h"
#include "src/snapshot/partial-serializer.h"
#include "src/snapshot/startup-serializer.h"


using namespace v8;

namespace {
void ReportUncaughtException(v8::Isolate* isolate,
                             const v8::TryCatch& try_catch) {
  CHECK(try_catch.HasCaught());
  v8::HandleScope handle_scope(isolate);
  std::string message =
      *v8::String::Utf8Value(isolate, try_catch.Message()->Get());
  int line = try_catch.Message()
                 ->GetLineNumber(isolate->GetCurrentContext())
                 .FromJust();
  std::string source_line = *v8::String::Utf8Value(
      isolate, try_catch.Message()
                   ->GetSourceLine(isolate->GetCurrentContext())
                   .ToLocalChecked());
  fprintf(stderr, "Unhandle exception: %s @%s[%d]\n", message.data(),
          source_line.data(), line);
}

} //namespace

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};

class SnapshotWriter {
 public:
  explicit SnapshotWriter(const char* snapshot_file)
      : fp_(GetFileDescriptorOrDie(snapshot_file))
        {}

  ~SnapshotWriter() {
    fclose(fp_);
  }

  void WriteSnapshot(void* buffer, int length) const {
    size_t written = fwrite(buffer, 1, length, fp_);
    if (written != static_cast<size_t>(length)) {
      i::PrintF("Writing snapshot file failed.. Aborting.\n");
      exit(1);
    }
  }

 private:

  FILE* GetFileDescriptorOrDie(const char* filename) {
    FILE* fp = base::OS::FOpen(filename, "wb");
    if (fp == NULL) {
      i::PrintF("Unable to open file \"%s\" for writing.\n", filename);
      exit(1);
    }
    return fp;
  }

  FILE* fp_;
};


int main(int argc, char** argv) {
  // By default, log code create information in the snapshot.
  i::FLAG_log_code = true;

  // Omit from the snapshot natives for features that can be turned off
  // at runtime.
  i::FLAG_harmony_shipping = true;

  i::FLAG_logfile_per_isolate = false;

  //i::FLAG_serialize_toplevel = true;
  i::FLAG_lazy = false;

  // Print the usage if an error occurs when parsing the command line
  // flags or if the help flag is set.
  int result = i::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (result > 0 || argc != 3 || i::FLAG_help) {
    ::printf("Usage: %s [flag] ... jsfile outfile\n", argv[0]);
    i::FlagList::PrintHelp();
    return !i::FLAG_help;
  }

  i::CpuFeatures::Probe(true);
  V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  std::unique_ptr<v8::Platform> platform(v8::platform::NewDefaultPlatform());
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  v8::Isolate::CreateParams create_params;
  ArrayBufferAllocator array_buffer_allocator;
  create_params.array_buffer_allocator = &array_buffer_allocator;
  v8::SnapshotCreator snapshot_creator;
  v8::Isolate* isolate = snapshot_creator.GetIsolate(); //v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Handle<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
    v8::Context::Scope scope(context);
    snapshot_creator.SetDefaultContext(context);

    FILE* file = v8::base::OS::FOpen(argv[1], "rb");
    if (file == NULL) {
      fprintf(stderr, "Failed to open '%s': errno %d\n", argv[1], errno);
      exit(1);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* chars = new char[size + 1];
    chars[size] = '\0';
    for (long i = 0; i < size;) {
      int read = static_cast<int>(fread(&chars[i], 1, size - i, file));
      if (read < 0) {
        fprintf(stderr, "Failed to read '%s': errno %d\n", argv[1], errno);
        exit(1);
      }
      i += read;
    }
    fclose(file);
    //Local<String> source_str = String::NewFromUtf8(isolate, chars);
    //Local<String> filename = String::NewFromUtf8(isolate, argv[1]);
    TryCatch try_catch(isolate);

    i::Isolate* iso = reinterpret_cast<i::Isolate*>(isolate);
    i::Handle<i::String> orig_source = iso->factory()
      ->NewStringFromUtf8(i::CStrVector(chars)).ToHandleChecked();

    ScriptCompiler::CachedData* cache = NULL;
    i::MaybeHandle<i::SharedFunctionInfo> maybe_func = i::Compiler::GetSharedFunctionInfoForScript(iso, orig_source,
                                                i::Compiler::ScriptDetails(),
                                                v8::ScriptOriginOptions(false, false, false, i::FLAG_nw_module),
                                                nullptr, nullptr,
                                                v8::ScriptCompiler::kEagerCompile,
                                                v8::ScriptCompiler::kNoCacheBecauseDeferredProduceCodeCache,
                                                i::NOT_NATIVES_CODE);
    if (try_catch.HasCaught()) {
      ReportUncaughtException(isolate, try_catch);
      fprintf(stderr, "Failure compiling '%s' (see above)\n", argv[1]);
      exit(1);
    }
    i::Handle<i::SharedFunctionInfo> func;
    maybe_func.ToHandle(&func);
    cache = i::CodeSerializer::Serialize(func);

    uint8_t* buffer = i::NewArray<uint8_t>(cache->length);
    i::MemCopy(buffer, cache->data, cache->length);

    SnapshotWriter writer(argv[2]);
    writer.WriteSnapshot(buffer, cache->length);
  }

  //snapshot_creator.CreateBlob(
  //                            v8::SnapshotCreator::FunctionCodeHandling::kClear);
  //V8::Dispose();
  //V8::ShutdownPlatform();
  return 0;
}

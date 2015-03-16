// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include "src/v8.h"

#include "include/libplatform/libplatform.h"
#include "src/assembler.h"
#include "src/base/platform/platform.h"
#include "src/bootstrapper.h"
#include "src/flags.h"
#include "src/list.h"
#include "src/natives.h"
#include "src/serialize.h"


using namespace v8;

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
  i::FLAG_harmony_shipping = false;

  i::FLAG_logfile_per_isolate = false;

  i::FLAG_serialize_toplevel = true;
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
  V8::InitializeICU();
  v8::Platform* platform = v8::platform::CreateDefaultPlatform();
  v8::V8::InitializePlatform(platform);
  v8::V8::Initialize();

  v8::Isolate* isolate = v8::Isolate::New();
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Handle<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
    v8::Context::Scope scope(context);

    FILE* file = v8::base::OS::FOpen(argv[1], "rb");
    if (file == NULL) {
      fprintf(stderr, "Failed to open '%s': errno %d\n", argv[1], errno);
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
        fprintf(stderr, "Failed to read '%s': errno %d\n", argv[1], errno);
        exit(1);
      }
      i += read;
    }
    fclose(file);
    Local<String> source_str = String::NewFromUtf8(isolate, chars);

    TryCatch try_catch;
    ScriptCompiler::Source script_source(source_str, ScriptOrigin(v8::Undefined(isolate)));
    ScriptCompiler::CompileUnbound(isolate, &script_source,
                                   v8::ScriptCompiler::kProduceCodeCache);
    if (try_catch.HasCaught()) {
      fprintf(stderr, "Failure compiling '%s' (see above)\n", argv[1]);
      exit(1);
    }

    if (script_source.GetCachedData()) {
      int length = script_source.GetCachedData()->length;
      uint8_t* cache = new uint8_t[length];
      memcpy(cache, script_source.GetCachedData()->data, length);

      SnapshotWriter writer(argv[2]);
      writer.WriteSnapshot(cache, length);
    }
  }

  V8::Dispose();
  V8::ShutdownPlatform();
  delete platform;
  return 0;
}

// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The common functionality when building with or without snapshots.

#include "src/v8.h"

#include "src/api.h"
#include "src/base/platform/platform.h"
#include "src/serialize.h"
#include "src/snapshot.h"

namespace v8 {
namespace internal {

static void ReserveSpaceForSnapshot(Deserializer* deserializer,
                                    struct NWSnapshotHeader* header) {

  deserializer->set_reservation(NEW_SPACE,         header->new_space_used);
  deserializer->set_reservation(OLD_POINTER_SPACE, header->pointer_space_used);
  deserializer->set_reservation(OLD_DATA_SPACE,    header->data_space_used);
  deserializer->set_reservation(CODE_SPACE,        header->code_space_used);
  deserializer->set_reservation(MAP_SPACE,         header->map_space_used);
  deserializer->set_reservation(CELL_SPACE,        header->cell_space_used);
  deserializer->set_reservation(PROPERTY_CELL_SPACE, header->property_space_used);

}

void Snapshot::ReserveSpaceForLinkedInSnapshot(Deserializer* deserializer) {
  deserializer->set_reservation(NEW_SPACE, new_space_used_);
  deserializer->set_reservation(OLD_POINTER_SPACE, pointer_space_used_);
  deserializer->set_reservation(OLD_DATA_SPACE, data_space_used_);
  deserializer->set_reservation(CODE_SPACE, code_space_used_);
  deserializer->set_reservation(MAP_SPACE, map_space_used_);
  deserializer->set_reservation(CELL_SPACE, cell_space_used_);
  deserializer->set_reservation(PROPERTY_CELL_SPACE,
                                property_cell_space_used_);
}


bool Snapshot::Initialize(const char* nw_snapshot_file) {
  if (nw_snapshot_file) {
    bool success = false;
    FILE* fp = fopen(nw_snapshot_file, "rb");
    CHECK_NE(NULL, fp);
    struct NWSnapshotHeader header;

    // read startup snapshot header
    if (!fread(&header, sizeof(header), 1, fp)) {
      fclose(fp);
      return false;
    }
    if (header.magic != 11801102) {
      fclose(fp);
      return false;
    }
    byte* str = NewArray<byte>(header.size);
    for (int i = 0; i < header.size && feof(fp) == 0;) {
      int read = static_cast<int>(fread(&str[i], 1, header.size - i, fp));
      if (read != (header.size - i) && ferror(fp) != 0) {
        fclose(fp);
        DeleteArray(str);
        return false;
      }
      i += read;
    }

    {
      SnapshotByteSource source(str, header.size);
      Deserializer deserializer(&source);
      ReserveSpaceForSnapshot(&deserializer, &header);
      success = V8::Initialize(&deserializer);
    }
    DeleteArray(str);
    if (!success) {
      fclose(fp);
      return false;
    }

    // read partial snapshot

    if (!fread(&header, sizeof(header), 1, fp)) {
      fclose(fp);
      return false;
    }
    if (header.magic != 11801102) {
      fclose(fp);
      return false;
    }

    context_new_space_used_     = header.new_space_used;
    context_pointer_space_used_ = header.pointer_space_used;
    context_data_space_used_    = header.data_space_used;
    context_code_space_used_    = header.code_space_used;
    context_map_space_used_     = header.map_space_used;
    context_cell_space_used_    = header.cell_space_used;

    str = NewArray<byte>(header.size);
    for (int i = 0; i < header.size && feof(fp) == 0;) {
      int read = static_cast<int>(fread(&str[i], 1, header.size - i, fp));
      if (read != (header.size - i) && ferror(fp) != 0) {
        fclose(fp);
        DeleteArray(str);
        return false;
      }
      i += read;
    }

    context_raw_size_ = header.size;
    context_size_     = header.size;
    context_raw_data_ = static_cast<const byte*>(str);

    fclose(fp);
    return true;

  } else if (size_ > 0) {
    ElapsedTimer timer;
    if (FLAG_profile_deserialization) {
      timer.Start();
    }
    SnapshotByteSource source(raw_data_, raw_size_);
    Deserializer deserializer(&source);
    ReserveSpaceForLinkedInSnapshot(&deserializer);
    bool success = V8::Initialize(&deserializer);
    if (FLAG_profile_deserialization) {
      double ms = timer.Elapsed().InMillisecondsF();
      PrintF("[Snapshot loading and deserialization took %0.3f ms]\n", ms);
    }
    return success;
  }
  return false;
}


bool Snapshot::HaveASnapshotToStartFrom() {
  return size_ != 0;
}


Handle<Context> Snapshot::NewContextFromSnapshot(Isolate* isolate) {
  if (context_size_ == 0) {
    return Handle<Context>();
  }
  SnapshotByteSource source(context_raw_data_,
                            context_raw_size_);
  Deserializer deserializer(&source);
  Object* root;
  deserializer.set_reservation(NEW_SPACE, context_new_space_used_);
  deserializer.set_reservation(OLD_POINTER_SPACE, context_pointer_space_used_);
  deserializer.set_reservation(OLD_DATA_SPACE, context_data_space_used_);
  deserializer.set_reservation(CODE_SPACE, context_code_space_used_);
  deserializer.set_reservation(MAP_SPACE, context_map_space_used_);
  deserializer.set_reservation(CELL_SPACE, context_cell_space_used_);
  deserializer.set_reservation(PROPERTY_CELL_SPACE,
                               context_property_cell_space_used_);
  deserializer.DeserializePartial(isolate, &root);
  CHECK(root->IsContext());
  return Handle<Context>(Context::cast(root));
}


#ifdef V8_USE_EXTERNAL_STARTUP_DATA
// Dummy implementations of Set*FromFile(..) APIs.
//
// These are meant for use with snapshot-external.cc. Should this file
// be compiled with those options we just supply these dummy implementations
// below. This happens when compiling the mksnapshot utility.
void SetNativesFromFile(StartupData* data) { CHECK(false); }
void SetSnapshotFromFile(StartupData* data) { CHECK(false); }
#endif  // V8_USE_EXTERNAL_STARTUP_DATA

} }  // namespace v8::internal

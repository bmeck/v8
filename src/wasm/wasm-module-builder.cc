// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/signature.h"

#include "src/handles.h"
#include "src/objects-inl.h"
#include "src/v8.h"
#include "src/zone/zone-containers.h"

#include "src/wasm/function-body-decoder.h"
#include "src/wasm/leb-helper.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-macro-gen.h"
#include "src/wasm/wasm-module-builder.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-opcodes.h"

#include "src/v8memory.h"

#if DEBUG
#define TRACE(...)                                    \
  do {                                                \
    if (FLAG_trace_wasm_encoder) PrintF(__VA_ARGS__); \
  } while (false)
#else
#define TRACE(...)
#endif

namespace v8 {
namespace internal {
namespace wasm {

// Emit a section code and the size as a padded varint that can be patched
// later.
size_t EmitSection(SectionCode code, ZoneBuffer& buffer) {
  // Emit the section code.
  buffer.write_u8(code);

  // Emit a placeholder for the length.
  return buffer.reserve_u32v();
}

// Patch the size of a section after it's finished.
void FixupSection(ZoneBuffer& buffer, size_t start) {
  buffer.patch_u32v(start, static_cast<uint32_t>(buffer.offset() - start -
                                                 kPaddedVarInt32Size));
}

WasmFunctionBuilder::WasmFunctionBuilder(WasmModuleBuilder* builder)
    : builder_(builder),
      locals_(builder->zone()),
      signature_index_(0),
      func_index_(static_cast<uint32_t>(builder->functions_.size())),
      body_(builder->zone()),
      name_(builder->zone()),
      exported_names_(builder->zone()),
      i32_temps_(builder->zone()),
      i64_temps_(builder->zone()),
      f32_temps_(builder->zone()),
      f64_temps_(builder->zone()),
      direct_calls_(builder->zone()),
      asm_offsets_(builder->zone(), 8) {}

void WasmFunctionBuilder::EmitVarInt(int32_t val) {
  byte buffer[5];
  byte* ptr = buffer;
  LEBHelper::write_i32v(&ptr, val);
  DCHECK_GE(5, ptr - buffer);
  body_.insert(body_.end(), buffer, ptr);
}

void WasmFunctionBuilder::EmitVarUint(uint32_t val) {
  byte buffer[5];
  byte* ptr = buffer;
  LEBHelper::write_u32v(&ptr, val);
  DCHECK_GE(5, ptr - buffer);
  body_.insert(body_.end(), buffer, ptr);
}

void WasmFunctionBuilder::SetSignature(FunctionSig* sig) {
  DCHECK(!locals_.has_sig());
  locals_.set_sig(sig);
  signature_index_ = builder_->AddSignature(sig);
}

uint32_t WasmFunctionBuilder::AddLocal(ValueType type) {
  DCHECK(locals_.has_sig());
  return locals_.AddLocals(1, type);
}

void WasmFunctionBuilder::EmitGetLocal(uint32_t local_index) {
  EmitWithVarUint(kExprGetLocal, local_index);
}

void WasmFunctionBuilder::EmitSetLocal(uint32_t local_index) {
  EmitWithVarUint(kExprSetLocal, local_index);
}

void WasmFunctionBuilder::EmitTeeLocal(uint32_t local_index) {
  EmitWithVarUint(kExprTeeLocal, local_index);
}

void WasmFunctionBuilder::EmitCode(const byte* code, uint32_t code_size) {
  for (size_t i = 0; i < code_size; ++i) {
    body_.push_back(code[i]);
  }
}

void WasmFunctionBuilder::Emit(WasmOpcode opcode) {
  body_.push_back(static_cast<byte>(opcode));
}

void WasmFunctionBuilder::EmitWithU8(WasmOpcode opcode, const byte immediate) {
  body_.push_back(static_cast<byte>(opcode));
  body_.push_back(immediate);
}

void WasmFunctionBuilder::EmitWithU8U8(WasmOpcode opcode, const byte imm1,
                                       const byte imm2) {
  body_.push_back(static_cast<byte>(opcode));
  body_.push_back(imm1);
  body_.push_back(imm2);
}

void WasmFunctionBuilder::EmitWithVarInt(WasmOpcode opcode, int32_t immediate) {
  body_.push_back(static_cast<byte>(opcode));
  EmitVarInt(immediate);
}

void WasmFunctionBuilder::EmitWithVarUint(WasmOpcode opcode,
                                          uint32_t immediate) {
  body_.push_back(static_cast<byte>(opcode));
  EmitVarUint(immediate);
}

void WasmFunctionBuilder::EmitI32Const(int32_t value) {
  EmitWithVarInt(kExprI32Const, value);
}

void WasmFunctionBuilder::EmitDirectCallIndex(uint32_t index) {
  DirectCallIndex call;
  call.offset = body_.size();
  call.direct_index = index;
  direct_calls_.push_back(call);
  byte code[] = {U32V_5(0)};
  EmitCode(code, sizeof(code));
}

void WasmFunctionBuilder::ExportAs(Vector<const char> name) {
  exported_names_.push_back(ZoneVector<char>(
      name.start(), name.start() + name.length(), builder_->zone()));
}

void WasmFunctionBuilder::SetName(Vector<const char> name) {
  name_.resize(name.length());
  memcpy(name_.data(), name.start(), name.length());
}

void WasmFunctionBuilder::AddAsmWasmOffset(int call_position,
                                           int to_number_position) {
  // We only want to emit one mapping per byte offset.
  DCHECK(asm_offsets_.size() == 0 || body_.size() > last_asm_byte_offset_);

  DCHECK_LE(body_.size(), kMaxUInt32);
  uint32_t byte_offset = static_cast<uint32_t>(body_.size());
  asm_offsets_.write_u32v(byte_offset - last_asm_byte_offset_);
  last_asm_byte_offset_ = byte_offset;

  DCHECK_GE(call_position, 0);
  asm_offsets_.write_i32v(call_position - last_asm_source_position_);

  DCHECK_GE(to_number_position, 0);
  asm_offsets_.write_i32v(to_number_position - call_position);
  last_asm_source_position_ = to_number_position;
}

void WasmFunctionBuilder::SetAsmFunctionStartPosition(int position) {
  DCHECK_EQ(0, asm_func_start_source_position_);
  DCHECK_LE(0, position);
  // Must be called before emitting any asm.js source position.
  DCHECK_EQ(0, asm_offsets_.size());
  asm_func_start_source_position_ = position;
  last_asm_source_position_ = position;
}

void WasmFunctionBuilder::DeleteCodeAfter(size_t position) {
  DCHECK_LE(position, body_.size());
  body_.resize(position);
}

void WasmFunctionBuilder::WriteSignature(ZoneBuffer& buffer) const {
  buffer.write_u32v(signature_index_);
}

void WasmFunctionBuilder::WriteExports(ZoneBuffer& buffer) const {
  for (auto name : exported_names_) {
    buffer.write_size(name.size());
    buffer.write(reinterpret_cast<const byte*>(name.data()), name.size());
    buffer.write_u8(kExternalFunction);
    buffer.write_size(func_index_ + builder_->function_imports_.size());
  }
}

void WasmFunctionBuilder::WriteBody(ZoneBuffer& buffer) const {
  size_t locals_size = locals_.Size();
  buffer.write_size(locals_size + body_.size());
  buffer.EnsureSpace(locals_size);
  byte** ptr = buffer.pos_ptr();
  locals_.Emit(*ptr);
  (*ptr) += locals_size;  // UGLY: manual bump of position pointer
  if (body_.size() > 0) {
    size_t base = buffer.offset();
    buffer.write(&body_[0], body_.size());
    for (DirectCallIndex call : direct_calls_) {
      buffer.patch_u32v(
          base + call.offset,
          call.direct_index +
              static_cast<uint32_t>(builder_->function_imports_.size()));
    }
  }
}

void WasmFunctionBuilder::WriteAsmWasmOffsetTable(ZoneBuffer& buffer) const {
  if (asm_func_start_source_position_ == 0 && asm_offsets_.size() == 0) {
    buffer.write_size(0);
    return;
  }
  size_t locals_enc_size = LEBHelper::sizeof_u32v(locals_.Size());
  size_t func_start_size =
      LEBHelper::sizeof_u32v(asm_func_start_source_position_);
  buffer.write_size(asm_offsets_.size() + locals_enc_size + func_start_size);
  // Offset of the recorded byte offsets.
  DCHECK_GE(kMaxUInt32, locals_.Size());
  buffer.write_u32v(static_cast<uint32_t>(locals_.Size()));
  // Start position of the function.
  buffer.write_u32v(asm_func_start_source_position_);
  buffer.write(asm_offsets_.begin(), asm_offsets_.size());
}

WasmModuleBuilder::WasmModuleBuilder(Zone* zone)
    : zone_(zone),
      signatures_(zone),
      function_imports_(zone),
      global_imports_(zone),
      functions_(zone),
      data_segments_(zone),
      indirect_functions_(zone),
      globals_(zone),
      signature_map_(zone),
      start_function_index_(-1) {}

WasmFunctionBuilder* WasmModuleBuilder::AddFunction(FunctionSig* sig) {
  functions_.push_back(new (zone_) WasmFunctionBuilder(this));
  // Add the signature if one was provided here.
  if (sig) functions_.back()->SetSignature(sig);
  return functions_.back();
}

void WasmModuleBuilder::AddDataSegment(const byte* data, uint32_t size,
                                       uint32_t dest) {
  data_segments_.push_back({ZoneVector<byte>(zone()), dest});
  ZoneVector<byte>& vec = data_segments_.back().data;
  for (uint32_t i = 0; i < size; i++) {
    vec.push_back(data[i]);
  }
}

bool WasmModuleBuilder::CompareFunctionSigs::operator()(FunctionSig* a,
                                                        FunctionSig* b) const {
  if (a->return_count() < b->return_count()) return true;
  if (a->return_count() > b->return_count()) return false;
  if (a->parameter_count() < b->parameter_count()) return true;
  if (a->parameter_count() > b->parameter_count()) return false;
  for (size_t r = 0; r < a->return_count(); r++) {
    if (a->GetReturn(r) < b->GetReturn(r)) return true;
    if (a->GetReturn(r) > b->GetReturn(r)) return false;
  }
  for (size_t p = 0; p < a->parameter_count(); p++) {
    if (a->GetParam(p) < b->GetParam(p)) return true;
    if (a->GetParam(p) > b->GetParam(p)) return false;
  }
  return false;
}

uint32_t WasmModuleBuilder::AddSignature(FunctionSig* sig) {
  SignatureMap::iterator pos = signature_map_.find(sig);
  if (pos != signature_map_.end()) {
    return pos->second;
  } else {
    uint32_t index = static_cast<uint32_t>(signatures_.size());
    signature_map_[sig] = index;
    signatures_.push_back(sig);
    return index;
  }
}

uint32_t WasmModuleBuilder::AllocateIndirectFunctions(uint32_t count) {
  uint32_t ret = static_cast<uint32_t>(indirect_functions_.size());
  indirect_functions_.resize(indirect_functions_.size() + count);
  return ret;
}

void WasmModuleBuilder::SetIndirectFunction(uint32_t indirect,
                                            uint32_t direct) {
  indirect_functions_[indirect] = direct;
}

uint32_t WasmModuleBuilder::AddImport(const char* name, int name_length,
                                      FunctionSig* sig) {
  function_imports_.push_back({AddSignature(sig), name, name_length});
  return static_cast<uint32_t>(function_imports_.size() - 1);
}

uint32_t WasmModuleBuilder::AddGlobalImport(const char* name, int name_length,
                                            ValueType type) {
  global_imports_.push_back(
      {WasmOpcodes::ValueTypeCodeFor(type), name, name_length});
  return static_cast<uint32_t>(global_imports_.size() - 1);
}

void WasmModuleBuilder::MarkStartFunction(WasmFunctionBuilder* function) {
  start_function_index_ = function->func_index();
}

uint32_t WasmModuleBuilder::AddGlobal(ValueType type, bool exported,
                                      bool mutability,
                                      const WasmInitExpr& init) {
  globals_.push_back({type, exported, mutability, init});
  return static_cast<uint32_t>(globals_.size() - 1);
}

void WasmModuleBuilder::WriteTo(ZoneBuffer& buffer) const {
  uint32_t exports = 0;

  // == Emit magic =============================================================
  TRACE("emit magic\n");
  buffer.write_u32(kWasmMagic);
  buffer.write_u32(kWasmVersion);

  // == Emit signatures ========================================================
  if (signatures_.size() > 0) {
    size_t start = EmitSection(kTypeSectionCode, buffer);
    buffer.write_size(signatures_.size());

    for (FunctionSig* sig : signatures_) {
      buffer.write_u8(kWasmFunctionTypeForm);
      buffer.write_size(sig->parameter_count());
      for (auto param : sig->parameters()) {
        buffer.write_u8(WasmOpcodes::ValueTypeCodeFor(param));
      }
      buffer.write_size(sig->return_count());
      for (auto ret : sig->returns()) {
        buffer.write_u8(WasmOpcodes::ValueTypeCodeFor(ret));
      }
    }
    FixupSection(buffer, start);
  }

  // == Emit imports ===========================================================
  if (global_imports_.size() + function_imports_.size() > 0) {
    size_t start = EmitSection(kImportSectionCode, buffer);
    buffer.write_size(global_imports_.size() + function_imports_.size());
    for (auto import : global_imports_) {
      buffer.write_u32v(0);                   // module name length
      buffer.write_u32v(import.name_length);  // field name length
      buffer.write(reinterpret_cast<const byte*>(import.name),  // field name
                   import.name_length);
      buffer.write_u8(kExternalGlobal);
      buffer.write_u8(import.type_code);
      buffer.write_u8(0);  // immutable
    }
    for (auto import : function_imports_) {
      buffer.write_u32v(0);                   // module name length
      buffer.write_u32v(import.name_length);  // field name length
      buffer.write(reinterpret_cast<const byte*>(import.name),  // field name
                   import.name_length);
      buffer.write_u8(kExternalFunction);
      buffer.write_u32v(import.sig_index);
    }
    FixupSection(buffer, start);
  }

  // == Emit function signatures ===============================================
  uint32_t num_function_names = 0;
  if (functions_.size() > 0) {
    size_t start = EmitSection(kFunctionSectionCode, buffer);
    buffer.write_size(functions_.size());
    for (auto function : functions_) {
      function->WriteSignature(buffer);
      exports += static_cast<uint32_t>(function->exported_names_.size());
      if (!function->name_.empty()) ++num_function_names;
    }
    FixupSection(buffer, start);
  }

  // == emit function table ====================================================
  if (indirect_functions_.size() > 0) {
    size_t start = EmitSection(kTableSectionCode, buffer);
    buffer.write_u8(1);  // table count
    buffer.write_u8(kWasmAnyFunctionTypeForm);
    buffer.write_u8(kResizableMaximumFlag);
    buffer.write_size(indirect_functions_.size());
    buffer.write_size(indirect_functions_.size());
    FixupSection(buffer, start);
  }

  // == emit memory declaration ================================================
  {
    size_t start = EmitSection(kMemorySectionCode, buffer);
    buffer.write_u8(1);  // memory count
    buffer.write_u32v(kResizableMaximumFlag);
    buffer.write_u32v(16);  // min memory size
    buffer.write_u32v(32);  // max memory size
    FixupSection(buffer, start);
  }

  // == Emit globals ===========================================================
  if (globals_.size() > 0) {
    size_t start = EmitSection(kGlobalSectionCode, buffer);
    buffer.write_size(globals_.size());

    for (auto global : globals_) {
      buffer.write_u8(WasmOpcodes::ValueTypeCodeFor(global.type));
      buffer.write_u8(global.mutability ? 1 : 0);
      switch (global.init.kind) {
        case WasmInitExpr::kI32Const: {
          DCHECK_EQ(kWasmI32, global.type);
          const byte code[] = {WASM_I32V_5(global.init.val.i32_const)};
          buffer.write(code, sizeof(code));
          break;
        }
        case WasmInitExpr::kI64Const: {
          DCHECK_EQ(kWasmI64, global.type);
          const byte code[] = {WASM_I64V_10(global.init.val.i64_const)};
          buffer.write(code, sizeof(code));
          break;
        }
        case WasmInitExpr::kF32Const: {
          DCHECK_EQ(kWasmF32, global.type);
          const byte code[] = {WASM_F32(global.init.val.f32_const)};
          buffer.write(code, sizeof(code));
          break;
        }
        case WasmInitExpr::kF64Const: {
          DCHECK_EQ(kWasmF64, global.type);
          const byte code[] = {WASM_F64(global.init.val.f64_const)};
          buffer.write(code, sizeof(code));
          break;
        }
        case WasmInitExpr::kGlobalIndex: {
          const byte code[] = {kExprGetGlobal,
                               U32V_5(global.init.val.global_index)};
          buffer.write(code, sizeof(code));
          break;
        }
        default: {
          // No initializer, emit a default value.
          switch (global.type) {
            case kWasmI32: {
              const byte code[] = {WASM_I32V_1(0)};
              buffer.write(code, sizeof(code));
              break;
            }
            case kWasmI64: {
              const byte code[] = {WASM_I64V_1(0)};
              buffer.write(code, sizeof(code));
              break;
            }
            case kWasmF32: {
              const byte code[] = {WASM_F32(0.0)};
              buffer.write(code, sizeof(code));
              break;
            }
            case kWasmF64: {
              const byte code[] = {WASM_F64(0.0)};
              buffer.write(code, sizeof(code));
              break;
            }
            default:
              UNREACHABLE();
          }
        }
      }
      buffer.write_u8(kExprEnd);
    }
    FixupSection(buffer, start);
  }

  // == emit exports ===========================================================
  if (exports > 0) {
    size_t start = EmitSection(kExportSectionCode, buffer);
    buffer.write_u32v(exports);
    for (auto function : functions_) function->WriteExports(buffer);
    FixupSection(buffer, start);
  }

  // == emit start function index ==============================================
  if (start_function_index_ >= 0) {
    size_t start = EmitSection(kStartSectionCode, buffer);
    buffer.write_size(start_function_index_ + function_imports_.size());
    FixupSection(buffer, start);
  }

  // == emit function table elements ===========================================
  if (indirect_functions_.size() > 0) {
    size_t start = EmitSection(kElementSectionCode, buffer);
    buffer.write_u8(1);              // count of entries
    buffer.write_u8(0);              // table index
    buffer.write_u8(kExprI32Const);  // offset
    buffer.write_u32v(0);
    buffer.write_u8(kExprEnd);
    buffer.write_size(indirect_functions_.size());  // element count

    for (auto index : indirect_functions_) {
      buffer.write_size(index + function_imports_.size());
    }

    FixupSection(buffer, start);
  }

  // == emit code ==============================================================
  if (functions_.size() > 0) {
    size_t start = EmitSection(kCodeSectionCode, buffer);
    buffer.write_size(functions_.size());
    for (auto function : functions_) {
      function->WriteBody(buffer);
    }
    FixupSection(buffer, start);
  }

  // == emit data segments =====================================================
  if (data_segments_.size() > 0) {
    size_t start = EmitSection(kDataSectionCode, buffer);
    buffer.write_size(data_segments_.size());

    for (auto segment : data_segments_) {
      buffer.write_u8(0);              // linear memory segment
      buffer.write_u8(kExprI32Const);  // initializer expression for dest
      buffer.write_u32v(segment.dest);
      buffer.write_u8(kExprEnd);
      buffer.write_u32v(static_cast<uint32_t>(segment.data.size()));
      buffer.write(&segment.data[0], segment.data.size());
    }
    FixupSection(buffer, start);
  }

  // == Emit names =============================================================
  if (num_function_names > 0 || !function_imports_.empty()) {
    // Emit the section code.
    buffer.write_u8(kUnknownSectionCode);
    // Emit a placeholder for the length.
    size_t start = buffer.reserve_u32v();
    // Emit the section string.
    buffer.write_size(4);
    buffer.write(reinterpret_cast<const byte*>("name"), 4);
    // Emit a subsection for the function names.
    buffer.write_u8(NameSectionType::kFunction);
    // Emit a placeholder for the subsection length.
    size_t functions_start = buffer.reserve_u32v();
    // Emit the function names.
    // Imports are always named.
    uint32_t num_imports = static_cast<uint32_t>(function_imports_.size());
    buffer.write_size(num_imports + num_function_names);
    uint32_t function_index = 0;
    for (; function_index < num_imports; ++function_index) {
      const WasmFunctionImport* import = &function_imports_[function_index];
      DCHECK_NOT_NULL(import->name);
      buffer.write_u32v(function_index);
      uint32_t name_len = static_cast<uint32_t>(import->name_length);
      buffer.write_u32v(name_len);
      buffer.write(reinterpret_cast<const byte*>(import->name), name_len);
    }
    if (num_function_names > 0) {
      for (auto function : functions_) {
        DCHECK_EQ(function_index,
                  function->func_index() + function_imports_.size());
        if (!function->name_.empty()) {
          buffer.write_u32v(function_index);
          buffer.write_size(function->name_.size());
          buffer.write(reinterpret_cast<const byte*>(function->name_.data()),
                       function->name_.size());
        }
        ++function_index;
      }
    }
    FixupSection(buffer, functions_start);
    FixupSection(buffer, start);
  }
}

void WasmModuleBuilder::WriteAsmJsOffsetTable(ZoneBuffer& buffer) const {
  // == Emit asm.js offset table ===============================================
  buffer.write_size(functions_.size());
  // Emit the offset table per function.
  for (auto function : functions_) {
    function->WriteAsmWasmOffsetTable(buffer);
  }
  // Append a 0 to indicate that this is an encoded table.
  buffer.write_u8(0);
}
}  // namespace wasm
}  // namespace internal
}  // namespace v8

#include "adapter.h"

#include "contract.h" // kAdapterVersion: generated from contract.json
#include "envelope.h" // own_json: records must own their strings

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <algorithm>
#include <iterator>
#include <string>

using namespace llvm;

namespace irez::adapter {
namespace {

std::string llvmDialectVersion() { return "llvm-" LLVM_VERSION_STRING; }

class Emitter {
public:
  // Records outlive the parsed module and the key tables, so every StringRef
  // leaf must be converted to an owning string on the way in.
  void emit(json::Object object) {
    result.records.push_back(std::move(*own_json(json::Value(std::move(object)))
                                             .getAsObject()));
  }
  void emit(std::initializer_list<json::Object::KV> properties) {
    emit(json::Object(properties));
  }
  Result result;
};

std::string printed(const Value &V) {
  std::string S;
  raw_string_ostream OS(S);
  V.print(OS);
  return OS.str();
}

std::string typeText(Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return OS.str();
}

std::string functionKey(unsigned Ordinal) { return "f" + std::to_string(Ordinal); }

json::Object capabilities() {
  return json::Object{
      {"record", "capabilities"},
      {"adapter", "llvm-ir"},
      {"adapter_version", kAdapterVersion},
      {"dialect_version", llvmDialectVersion()},
      {"llvm_build_version", LLVM_VERSION_STRING},
      {"llvm_ir_reader_version", LLVM_VERSION_STRING},
      {"capabilities",
       json::Object{
           {"entity_index", json::Object{{"status", "supported"}, {"precision", "exact"}}},
           {"operand_graph", json::Object{{"status", "supported"}, {"precision", "exact"}}},
           {"cfg", json::Object{{"status", "supported"}, {"precision", "exact"}}},
           {"control_dependence", json::Object{{"status", "supported"}, {"precision", "exact"}}},
           {"source_mapping", json::Object{{"status", "supported"}, {"precision", "partial"}}},
           {"direct_calls", json::Object{{"status", "supported"}, {"precision", "exact"}}},
           {"indirect_calls", json::Object{{"status", "partial"}, {"precision", "conservative"}}},
           {"memory_dependencies", json::Object{{"status", "unsupported"}}},
           {"path_conditions", json::Object{{"status", "unsupported"}}},
           {"runtime_events", json::Object{{"status", "unsupported"}}}}}};
}

std::unique_ptr<Module> loadModule(LLVMContext &Context, const std::string &Input,
                                   Emitter &E) {
  SMDiagnostic Error;
  auto M = parseIRFile(Input, Error, Context);
  if (!M) {
    std::string Message;
    raw_string_ostream OS(Message);
    Error.print("irez-llvm-index", OS);
    E.emit({{"record", "error"}, {"message", OS.str()}});
  }
  return M;
}

std::string bodyFingerprint(const Function &F) {
  std::string Body;
  raw_string_ostream OS(Body);
  F.print(OS);
  auto Digest = SHA256::hash(arrayRefFromStringRef(OS.str()));
  return toHex(Digest);
}

json::Object instructionAttributes(const Instruction &I) {
  json::Object A;
  if (auto *O = dyn_cast<OverflowingBinaryOperator>(&I)) {
    A["nsw"] = O->hasNoSignedWrap();
    A["nuw"] = O->hasNoUnsignedWrap();
  }
  if (auto *E = dyn_cast<PossiblyExactOperator>(&I))
    A["exact"] = E->isExact();
  if (auto *FPO = dyn_cast<FPMathOperator>(&I)) {
    FastMathFlags FMF = FPO->getFastMathFlags();
    A["fast"] = FMF.isFast();
    A["nnan"] = FMF.noNaNs();
    A["ninf"] = FMF.noInfs();
    A["nsz"] = FMF.noSignedZeros();
    A["arcp"] = FMF.allowReciprocal();
    A["contract"] = FMF.allowContract();
    A["afn"] = FMF.approxFunc();
    A["reassoc"] = FMF.allowReassoc();
  }
  if (auto *CB = dyn_cast<CallBase>(&I)) {
    A["calling_convention"] = static_cast<int64_t>(CB->getCallingConv());
    std::string Attrs;
    raw_string_ostream OS(Attrs);
    CB->getAttributes().print(OS);
    A["call_attributes"] = OS.str();
  }
  return A;
}

json::Object successorAttributes(const Instruction &I, unsigned ordinal) {
  json::Object attributes;
  if (auto *branch = dyn_cast<BranchInst>(&I); branch && branch->isConditional())
    attributes["condition_value"] = (ordinal == 0);
  if (auto *switch_inst = dyn_cast<SwitchInst>(&I)) {
    if (ordinal == 0) {
      attributes["switch_default"] = true;
    } else {
      auto case_it = switch_inst->case_begin();
      std::advance(case_it, ordinal - 1);
      attributes["switch_case"] = printed(*case_it->getCaseValue());
    }
  }
  return attributes;
}

void emitSource(Emitter &E, const Instruction &I, StringRef LocalKey) {
  const DILocation *Location = I.getDebugLoc().get();
  unsigned Depth = 0;
  while (Location) {
    std::string File;
    std::string ScopeName;
    // The scope of a well-formed DILocation is never null, but malformed
    // debug metadata must not crash the adapter (V00_00 bug B10).
    if (const DIScope *Scope = Location->getScope()) {
      if (auto *F = Scope->getFile()) {
        SmallString<256> full(F->getDirectory());
        if (!F->getFilename().empty())
          sys::path::append(full, F->getFilename());
        File = full.empty() ? F->getFilename().str() : full.str().str();
        // Source paths are part of the cross-platform response contract:
        // sys::path::append uses the native separator, so a Windows build
        // would emit mixed separators (".../fixtures\nonfloating.c") where
        // Linux emits pure POSIX. Canonicalize to forward slashes.
        std::replace(File.begin(), File.end(), '\\', '/');
      }
      ScopeName = Scope->getName().str();
    }
    E.emit({{"record", "source"},
            {"entity", LocalKey},
            {"file", File},
            {"line", static_cast<int64_t>(Location->getLine())},
            {"column", static_cast<int64_t>(Location->getColumn())},
            {"discriminator", static_cast<int64_t>(Location->getDiscriminator())},
            {"inline_depth", static_cast<int64_t>(Depth)},
            {"scope", ScopeName}});
    Location = Location->getInlinedAt();
    ++Depth;
  }
}

void emitModuleHeader(Emitter &E, const Module &M) {
  E.emit(capabilities());
  E.emit({{"record", "module"},
          {"identifier", std::string(M.getModuleIdentifier())},
          {"source_file", std::string(M.getSourceFileName())},
          {"target_triple", M.getTargetTriple().str()},
          {"data_layout", std::string(M.getDataLayoutStr())}});
}

} // namespace

json::Object version_info() {
  return json::Object{{"adapter", "llvm-ir"},
                      {"adapter_version", kAdapterVersion},
                      {"dialect_version", llvmDialectVersion()},
                      {"llvm_build_version", LLVM_VERSION_STRING},
                      {"llvm_ir_reader_version", LLVM_VERSION_STRING}};
}

Result catalog(const std::string &input_path) {
  Emitter E;
  LLVMContext Context;
  auto M = loadModule(Context, input_path, E);
  if (!M)
    return std::move(E.result);
  emitModuleHeader(E, *M);
  unsigned Ordinal = 0;
  for (const Function &F : *M) {
    E.emit({{"record", "function"},
            {"key", functionKey(Ordinal)},
            {"ordinal", static_cast<int64_t>(Ordinal++)},
            {"name", F.getName()},
            {"declaration", F.isDeclaration()},
            {"linkage", std::to_string(static_cast<unsigned>(F.getLinkage()))},
            {"signature", typeText(F.getFunctionType())},
            {"body_fingerprint", F.isDeclaration() ? "" : bodyFingerprint(F)}});
  }
  return std::move(E.result);
}

Result function_graph(const std::string &input_path, const std::string &function_key) {
  Emitter E;
  LLVMContext Context;
  auto M = loadModule(Context, input_path, E);
  if (!M)
    return std::move(E.result);
  emitModuleHeader(E, *M);

  Function *Selected = nullptr;
  unsigned FunctionOrdinal = 0;
  unsigned Current = 0;
  for (Function &F : *M) {
    const std::string Key = functionKey(Current);
    if (Key == function_key || F.getName() == function_key) {
      Selected = &F;
      FunctionOrdinal = Current;
      break;
    }
    ++Current;
  }
  if (!Selected) {
    E.emit({{"record", "error"}, {"message", "function key not found"}});
    return std::move(E.result);
  }

  const std::string FKey = "function:" + functionKey(FunctionOrdinal);
  E.emit({{"record", "entity"}, {"kind", "function"}, {"local_key", FKey},
          {"ordinal", static_cast<int64_t>(FunctionOrdinal)},
          {"name", Selected->getName()},
          {"llvm_type", typeText(Selected->getFunctionType())},
          {"materialization", "structure_ready"}, {"exact_text", printed(*Selected)}});

  DenseMap<const Value *, std::string> Keys;
  unsigned CatalogOrdinal = 0;
  for (Function &F : *M)
    Keys[&F] = "function:" + functionKey(CatalogOrdinal++);
  unsigned ArgOrdinal = 0;
  for (Argument &A : Selected->args()) {
    std::string Key = "arg:" + functionKey(FunctionOrdinal) + ":" +
                      std::to_string(ArgOrdinal);
    Keys[&A] = Key;
    E.emit({{"record", "entity"}, {"kind", "argument"}, {"local_key", Key},
            {"ordinal", static_cast<int64_t>(ArgOrdinal++)}, {"name", A.getName()},
            {"llvm_type", typeText(A.getType())}, {"materialization", "structure_ready"},
            {"exact_text", printed(A)}});
  }

  unsigned BlockOrdinal = 0, InstructionOrdinal = 0;
  for (BasicBlock &BB : *Selected) {
    std::string Key = "block:" + functionKey(FunctionOrdinal) + ":" +
                      std::to_string(BlockOrdinal);
    Keys[&BB] = Key;
    E.emit({{"record", "entity"}, {"kind", "basic_block"}, {"local_key", Key},
            {"ordinal", static_cast<int64_t>(BlockOrdinal++)}, {"name", BB.getName()},
            {"llvm_type", "label"}, {"materialization", "structure_ready"}});
    for (Instruction &I : BB) {
      std::string IKey = "inst:" + functionKey(FunctionOrdinal) + ":" +
                         std::to_string(InstructionOrdinal);
      Keys[&I] = IKey;
      E.emit({{"record", "entity"}, {"kind", "instruction"}, {"local_key", IKey},
              {"ordinal", static_cast<int64_t>(InstructionOrdinal++)},
              {"name", I.getName()}, {"opcode", I.getOpcodeName()},
              {"llvm_type", typeText(I.getType())},
              {"materialization", "structure_ready"}, {"exact_text", printed(I)},
              {"attributes", instructionAttributes(I)}});
    }
  }

  DenseMap<const Value *, std::string> ExtraKeys;
  unsigned ConstantOrdinal = 0;
  auto ensureOperand = [&](const Value *V) -> std::string {
    if (auto It = Keys.find(V); It != Keys.end())
      return It->second;
    if (auto It = ExtraKeys.find(V); It != ExtraKeys.end())
      return It->second;
    std::string Key;
    StringRef Kind;
    if (isa<GlobalVariable>(V)) {
      unsigned GlobalOrdinal = 0;
      for (const GlobalVariable &G : M->globals()) {
        if (&G == V)
          break;
        ++GlobalOrdinal;
      }
      Key = "global:g" + std::to_string(GlobalOrdinal);
      Kind = "global";
    } else {
      Key = "constant:" + functionKey(FunctionOrdinal) + ":" +
            std::to_string(ConstantOrdinal++);
      Kind = "constant";
    }
    ExtraKeys[V] = Key;
    E.emit({{"record", "entity"}, {"kind", Kind}, {"local_key", Key},
            {"name", V->hasName() ? V->getName() : StringRef()},
            {"llvm_type", typeText(V->getType())}, {"materialization", "structure_ready"},
            {"exact_text", printed(*V)}});
    return Key;
  };

  for (Argument &A : Selected->args())
    E.emit({{"record", "relation"}, {"kind", "llvm.contains"}, {"src", FKey},
            {"dst", Keys[&A]}, {"modality", "must"}, {"precision", "exact"}});
  for (BasicBlock &BB : *Selected) {
    E.emit({{"record", "relation"}, {"kind", "llvm.contains"}, {"src", FKey},
            {"dst", Keys[&BB]}, {"modality", "must"}, {"precision", "exact"}});
    for (Instruction &I : BB) {
      E.emit({{"record", "relation"}, {"kind", "llvm.contains"}, {"src", Keys[&BB]},
              {"dst", Keys[&I]}, {"modality", "must"}, {"precision", "exact"}});
      for (unsigned O = 0; O < I.getNumOperands(); ++O) {
        Value *Operand = I.getOperand(O);
        json::Object Attributes;
        if (auto *Phi = dyn_cast<PHINode>(&I))
          if (O < Phi->getNumIncomingValues())
            Attributes["incoming_block"] = Keys[Phi->getIncomingBlock(O)];
        E.emit({{"record", "relation"}, {"kind", "llvm.operand"}, {"src", Keys[&I]},
                {"dst", ensureOperand(Operand)}, {"ordinal", static_cast<int64_t>(O)},
                {"modality", "must"}, {"precision", "exact"},
                {"attributes", std::move(Attributes)}});
        if (isa<GlobalVariable>(Operand))
          E.emit({{"record", "relation"}, {"kind", "llvm.references-global"},
                  {"src", Keys[&I]}, {"dst", ensureOperand(Operand)},
                  {"modality", "must"}, {"precision", "exact"}});
      }
      if (I.isTerminator()) {
        unsigned S = 0;
        for (BasicBlock *Successor : successors(&BB)) {
          json::Object EdgeAttributes = successorAttributes(I, S);
          E.emit({{"record", "relation"}, {"kind", "llvm.cfg-successor"},
                  {"src", Keys[&I]}, {"dst", Keys[Successor]},
                  {"ordinal", static_cast<int64_t>(S++)},
                  {"modality", "must"}, {"precision", "exact"},
                  {"attributes", std::move(EdgeAttributes)}});
        }
      }
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Value *called = CB->getCalledOperand()->stripPointerCasts();
        const Function *Callee = dyn_cast<Function>(called);
        if (!Callee)
          if (auto *alias = dyn_cast<GlobalAlias>(called))
            Callee = dyn_cast_or_null<Function>(alias->getAliaseeObject());
        if (Callee) {
          std::string Target = ensureOperand(Callee);
          E.emit({{"record", "relation"}, {"kind", "llvm.calls"}, {"src", Keys[&I]},
                  {"dst", Target}, {"modality", "must"}, {"precision", "exact"},
                  {"attributes", json::Object{{"external", Callee->isDeclaration()}}}});
        }
      }
      emitSource(E, I, Keys[&I]);
    }
  }

  // Control dependence via the post-dominator tree.  For each CFG edge
  // BB->Successor that is not post-dominated by Successor, walk from the
  // successor towards BB's immediate post-dominator.  Every block on that
  // path is controlled by the edge.  Testing only
  // PDT.dominates(Successor,Y) misses multi-block branch arms.
  PostDominatorTree PDT;
  PDT.recalculate(*Selected);
  for (BasicBlock &BB : *Selected) {
    Instruction *Term = BB.getTerminator();
    auto *BranchNode = PDT.getNode(&BB);
    auto *Stop = BranchNode ? BranchNode->getIDom() : nullptr;
    unsigned S = 0;
    for (BasicBlock *Successor : successors(&BB)) {
      if (!PDT.dominates(Successor, &BB)) {
        auto *Runner = PDT.getNode(Successor);
        while (Runner && Runner != Stop) {
          BasicBlock *Y = Runner->getBlock();
          if (Y && Y != &BB)
            E.emit({{"record", "relation"},
                    {"kind", "llvm.control-dependence"},
                    {"src", Keys[Y]},
                    {"dst", Keys[Term]},
                    {"ordinal", static_cast<int64_t>(S)},
                    {"modality", "must"},
                    {"precision", "exact"},
                    {"attributes", [&] {
                       json::Object attributes = successorAttributes(*Term, S);
                       attributes["successor"] = Keys[Successor];
                       return attributes;
                     }()}});
          Runner = Runner->getIDom();
        }
      }
      ++S;
    }
  }
  return std::move(E.result);
}

} // namespace irez::adapter

// hokkaido — LLVM-based compiler
// Compiles .hk (Hokkaido) source files to native code via LLVM.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include "borrow_checker.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

using namespace llvm;

// =========================================================================
// Minimal C runtime discovery for linking with ld.lld directly
// =========================================================================
//
// ld.lld is purely a linker — unlike clang, it has no built-in knowledge of
// where a given system keeps its C runtime startup objects (crt1.o,
// crti.o, crtn.o), libc, or the dynamic loader path. A compiler driver
// like clang normally works that out for you from the target triple and
// sysroot. Here we do a small best-effort search across common distro
// layouts instead, so the toolchain doesn't need a full clang install just
// to perform the final link step. Anything nonstandard can be overridden
// via HOKKAIDO_CRT_DIR / HOKKAIDO_DYNAMIC_LINKER.

static std::string find_crt_dir() {
  if (const char *env = std::getenv("HOKKAIDO_CRT_DIR")) return env;
  static const char *candidates[] = {
      "/usr/lib/x86_64-linux-gnu",   // Debian/Ubuntu x86_64
      "/usr/lib/aarch64-linux-gnu",  // Debian/Ubuntu arm64
      "/usr/lib64",                  // Fedora/RHEL/openSUSE
      "/usr/lib",                    // Arch and others
      "/lib64",
      "/lib",
  };
  for (const char *dir : candidates) {
    if (std::filesystem::exists(std::string(dir) + "/crt1.o")) return dir;
  }
  return "";
}

static bool find_ld_lld() {
  if (const char *env = std::getenv("HOKKAIDO_LD_LLD")) {
    return std::filesystem::exists(env);
  }
  if (const char *path = std::getenv("PATH")) {
    std::string PathStr(path);
    size_t start = 0;
    while (start <= PathStr.size()) {
      size_t end = PathStr.find(':', start);
      if (end == std::string::npos) end = PathStr.size();
      std::string dir = PathStr.substr(start, end - start);
      if (!dir.empty() && std::filesystem::exists(dir + "/ld.lld")) {
        return true;
      }
      start = end + 1;
    }
  }
  return false;
}

static std::string find_dynamic_linker() {
  if (const char *env = std::getenv("HOKKAIDO_DYNAMIC_LINKER")) return env;
  static const char *candidates[] = {
      "/lib64/ld-linux-x86-64.so.2",   // glibc x86_64
      "/lib/ld-linux-aarch64.so.1",    // glibc arm64
      "/lib/ld-linux.so.2",            // glibc i386
      "/lib/ld-musl-x86_64.so.1",      // musl x86_64
  };
  for (const char *path : candidates) {
    if (std::filesystem::exists(path)) return path;
  }
  return "";
}

// =========================================================================
// Main entry point
// =========================================================================

void print_usage() {
  std::cout << "hokkaido — LLVM-based compiler\n\n";
  std::cout << "Usage:\n";
  std::cout << "  hokkaido input.hk                  Print LLVM IR to stdout\n";
  std::cout << "  hokkaido input.hk -o output         Compile to an object file (output.o)\n";
  std::cout << "  hokkaido input.hk -o output -O2     Compile with optimizations (O0, O1, O2, O3, Os, Oz)\n";
  std::cout << "  hokkaido input.hk -o output --freestanding\n";
  std::cout << "                                       Same, but with no CRT/libc dependency:\n";
  std::cout << "                                       'main' becomes the raw ELF entry point\n";
  std::cout << "                                       and exits via a direct syscall. Plain\n";
  std::cout << "                                       'extern fn' declarations are rejected,\n";
  std::cout << "                                       since there's no libc to resolve them.\n";
  std::cout << "  hokkaido input.hk -o output --target wasm32-unknown-wasi\n";
  std::cout << "                                       Compile to WebAssembly object file\n";
  std::cout << "                                       (requires wasm-ld for linking)\n";
  std::cout << "  hokkaido input.hk -o output --target wasm32-unknown-unknown\n";
  std::cout << "                                       Compile to bare WebAssembly object file\n";
  std::cout << "Optimization levels:\n";
  std::cout << "  -O0    No optimization (default, debug-friendly)\n";
  std::cout << "  -O1    Light optimization, preserves debuggability\n";
  std::cout << "  -O2    Standard optimizations (recommended for release)\n";
  std::cout << "  -O3    Aggressive optimizations\n";
  std::cout << "  -Os    Optimize for code size\n";
  std::cout << "  -Oz    Aggressively optimize for code size\n";
  std::cout << "  -O     Alias for -O2\n\n";
  std::cout << "Target triples:\n";
  std::cout << "  --target wasm32-unknown-wasi       WebAssembly with WASI support\n";
  std::cout << "  --target wasm32-unknown-unknown    Bare WebAssembly (no OS)\n";
  std::cout << "  --target <triple>                  Any LLVM-supported target\n\n";
  std::cout << "hokkaido does not link executables itself — it only emits an object\n";
  std::cout << "file. After compiling, link it yourself with 'ld.lld', 'clang', or your\n";
  std::cout << "platform's usual linker. Run with -o to see a suggested link command for\n";
  std::cout << "this object file once it's been emitted.\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage();
    return 0;
  }

  std::filesystem::path filePath(argv[1]);
  std::ifstream ifs(filePath);
  if (!ifs) {
    std::cerr << "Error: cannot open file '" << argv[1] << "'\n";
    return 1;
  }
  std::string Content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());

  // Initialize LLVM targets
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllAsmPrinters();

  // Parse optional -o flag, --freestanding, --target, optimization flags, and any extra
  // linker flags (e.g. -lm, -lcurl, -L/path) for linking against C libraries
  // used by `extern fn` declarations.
  std::string OutputPath;
  bool Freestanding = false;
  std::string TargetTriple;
  std::vector<std::string> ExtraLinkArgs;
  // Parse optimization level (default: -O0 — no optimization, debug-friendly)
  CodeGenOptLevel OptLevel = CodeGenOptLevel::None;
  OptimizationLevel LLVMOptLevel = OptimizationLevel::O0;
  for (int i = 2; i < argc; i++) {
    std::string Arg = argv[i];
    if (Arg == "-o" && i + 1 < argc) {
      OutputPath = argv[++i];
    } else if (Arg == "--freestanding") {
      Freestanding = true;
    } else if (Arg == "--target" && i + 1 < argc) {
      TargetTriple = argv[++i];
    } else if (Arg == "-O") {
      OptLevel = CodeGenOptLevel::Default;
      LLVMOptLevel = OptimizationLevel::O2;
    } else if (Arg.rfind("-O", 0) == 0) {
      std::string Level = Arg.substr(2);
      if (Level == "0") {
        OptLevel = CodeGenOptLevel::None;
        LLVMOptLevel = OptimizationLevel::O0;
      } else if (Level == "1") {
        OptLevel = CodeGenOptLevel::Less;
        LLVMOptLevel = OptimizationLevel::O1;
      } else if (Level == "2") {
        OptLevel = CodeGenOptLevel::Default;
        LLVMOptLevel = OptimizationLevel::O2;
      } else if (Level == "3") {
        OptLevel = CodeGenOptLevel::Aggressive;
        LLVMOptLevel = OptimizationLevel::O3;
      } else if (Level == "s") {
        OptLevel = CodeGenOptLevel::Default;
        LLVMOptLevel = OptimizationLevel::Os;
      } else if (Level == "z") {
        OptLevel = CodeGenOptLevel::Default;
        LLVMOptLevel = OptimizationLevel::Oz;
      } else {
        std::cerr << "Warning: unknown optimization level '" << Arg << "', ignoring\n";
      }
    } else {
      ExtraLinkArgs.push_back(Arg);
    }
  }

  // Use specified target or default to host
  std::string TargetTripleStr = TargetTriple.empty() ? sys::getDefaultTargetTriple() : TargetTriple;
  Triple TheTriple(TargetTripleStr);
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(TargetTripleStr, Error);
  if (!TheTarget) {
    errs() << "Error: " << Error;
    return 1;
  }

  // Check if this is a WebAssembly target
  bool IsWasmTarget = TheTriple.isWasm();
  if (IsWasmTarget) {
    Freestanding = true; // Wasm targets don't have libc by default
  }

  std::string CPU = "generic";
  TargetOptions opt;
  // Default to position-independent code. Modern clang/ld.lld link PIE
  // executables by default, which requires the object file's relocations
  // to be PIC-safe; leaving this unset previously meant LLVM picked a
  // static/non-PIC model, producing relocations (e.g. against .rodata
  // string literals) that a PIE link would reject with errors like
  // "relocation R_X86_64_32 ... can not be used when making a PIE
  // object". Building PIC here means `clang main.o -o main` works without
  // the caller needing to pass `-no-pie` themselves.
  auto RM = std::optional<Reloc::Model>(Reloc::Model::PIC_);

  // WebAssembly uses different relocation model
  if (IsWasmTarget) {
    RM = std::nullopt; // WebAssembly doesn't use PIC
  }

  std::unique_ptr<TargetMachine> TM(
      TheTarget->createTargetMachine(TheTriple, CPU, "", opt, RM,
                                     std::nullopt, OptLevel));

  LLVMContext Context;
  std::unique_ptr<Module> M = std::make_unique<Module>("hokkaido", Context);
  M->setDataLayout(TM->createDataLayout());
  M->setTargetTriple(TheTriple);

  IRBuilder<> Builder(Context);

  // Handle .hk files (hokkaido language)
  if (filePath.extension() == ".hk") {
    Lexer lexer(Content);
    auto included_files = std::make_shared<std::set<std::string>>();
    auto imported_packages = std::make_shared<std::set<std::string>>();
    {
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(filePath, ec);
      included_files->insert((ec ? filePath : canonical).string());
    }
    Parser parser(lexer, filePath.string(), filePath.parent_path().string(), included_files, imported_packages, Content);
    auto decls = parser.parse_program();

    if (!parser.ok()) {
      std::cerr << parser.error() << "\n";
      return 1;
    }

    // Run NLL borrow checker on all function declarations
    {
      NLLBorrowChecker bc;
      bc.set_declarations(decls);
      for (auto &decl : decls) {
        if (auto *fn = dynamic_cast<FnDecl *>(decl.get())) {
          if (!fn->is_extern) {
            if (!bc.check_fn(fn->name, fn)) {
              return 1;
            }
          }
        }
      }
      // Run NLL borrow checker on impl method declarations
      for (auto &decl : decls) {
        if (auto *impl = dynamic_cast<ImplDecl *>(decl.get())) {
          for (auto &method : impl->methods) {
            if (!bc.check_fn(method->name, method.get())) {
              return 1;
            }
          }
        }
      }
    }

    CodeGen cg(Context, *M, Builder, Freestanding);
    cg.set_source_text(Content);
    if (!cg.generate(decls)) {
      return 1;
    }

    if (OutputPath.empty()) {
      M->print(outs(), nullptr);
    } else {
      std::string ObjPath = OutputPath + ".o";
      std::error_code EC;
      raw_fd_ostream Dest(ObjPath, EC, sys::fs::OF_None);
      if (EC) {
        errs() << "Error: cannot open '" << ObjPath << "': " << EC.message() << "\n";
        return 1;
      }

      LoopAnalysisManager LAM;
      FunctionAnalysisManager FAM;
      CGSCCAnalysisManager CGAM;
      ModuleAnalysisManager MAM;

      PassBuilder PB(TM.get());
      PB.registerModuleAnalyses(MAM);
      PB.registerCGSCCAnalyses(CGAM);
      PB.registerFunctionAnalyses(FAM);
      PB.registerLoopAnalyses(LAM);
      PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

      // Register target-specific pass builder callbacks before building
      // any optimization pipeline.
      TM->registerPassBuilderCallbacks(PB);

      // Run LLVM optimization passes when optimization level > 0
      if (OptLevel != CodeGenOptLevel::None) {
        ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(LLVMOptLevel);
        MPM.run(*M, MAM);
      }

      // TargetMachine::addPassesToEmitFile still requires the legacy PM
      // for the codegen backend step.
      legacy::PassManager LPM;
      if (TM->addPassesToEmitFile(LPM, Dest, nullptr, CodeGenFileType::ObjectFile)) {
        errs() << "Error: target does not support object emission\n";
        return 1;
      }
      LPM.run(*M);
      Dest.close();

      bool HaveLdLld = find_ld_lld();

      std::string CrtDir = find_crt_dir();
      std::string DynamicLinker = find_dynamic_linker();

      
    }
    return 0;
  }

  std::cerr << "Unsupported file type: " << filePath.extension() << "\n";
  std::cerr << "Supported: .hk (hokkaido)\n";
  return 1;
}

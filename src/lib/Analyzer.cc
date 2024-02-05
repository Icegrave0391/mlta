//===-- Analyzer.cc - the kernel-analysis framework-------------===//
//
// It constructs a global call-graph based on multi-layer type
// analysis.
//
//===-----------------------------------------------------------===//

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Path.h"

#include <memory>
#include <vector>
#include <sstream>
#include <sys/resource.h>
#include <iomanip>

#include "Analyzer.h"
#include "CallGraph.h"
#include "Config.h"

using namespace llvm;

// Command line parameters.
cl::list<std::string> InputFilenames(
    cl::Positional, cl::OneOrMore, cl::desc("<input bitcode files>"));

cl::opt<unsigned> VerboseLevel(
    "verbose-level", cl::desc("Print information at which verbose level"),
    cl::init(0));

cl::opt<int> MLTA(
    "mlta",
  cl::desc("Multi-layer type analysis for refining indirect-call \
    targets"),
  cl::NotHidden, cl::init(2));

GlobalContext GlobalCtx;


void IterativeModulePass::run(ModuleList &modules) {

	ModuleList::iterator i, e;
	OP << "[" << ID << "] Initializing " << modules.size() << " modules ";
	bool again = true;
	while (again) {
		again = false;
		for (i = modules.begin(), e = modules.end(); i != e; ++i) {
			again |= doInitialization(i->first);
			OP << ".";
		}
	}
	OP << "\n";

	unsigned iter = 0, changed = 1;
	while (changed) {
		++iter;
		changed = 0;
		unsigned counter_modules = 0;
		unsigned total_modules = modules.size();
		for (i = modules.begin(), e = modules.end(); i != e; ++i) {
			OP << "[" << ID << " / " << iter << "] ";
			OP << "[" << ++counter_modules << " / " << total_modules << "] ";
			OP << "[" << i->second << "]\n";

			bool ret = doModulePass(i->first);
			if (ret) {
				++changed;
				OP << "\t [CHANGED]\n";
			} else
				OP << "\n";
		}
		OP << "[" << ID << "] Updated in " << changed << " modules.\n";
	}

	OP << "[" << ID << "] Postprocessing ...\n";
	again = true;
	while (again) {
		again = false;
		for (i = modules.begin(), e = modules.end(); i != e; ++i) {
			// TODO: Dump the results.
			again |= doFinalization(i->first);
		}
	}

	OP << "[" << ID << "] Done!\n\n";
}

void PrintResults(GlobalContext *GCtx) {

	int TotalTargets = 0;
	for (auto IC : GCtx->IndirectCallInsts) {
		TotalTargets += GCtx->Callees[IC].size();
	}
	float AveIndirectTargets = 0.0;
	if (GCtx->NumValidIndirectCalls)
		AveIndirectTargets =
			(float)GCtx->NumIndirectCallTargets/GCtx->IndirectCallInsts.size();

	int totalsize = 0;
	for (auto &curEle: GCtx->Callees) {
		if (curEle.first->isIndirectCall()) {
			totalsize += curEle.second.size();
		}
	}
	OP << "\n@@ Total number of final callees: " << totalsize << ".\n";

	OP<<"############## Result Statistics ##############\n";
	//cout<<"# Ave. Number of indirect-call targets: \t"<<std::setprecision(5)<<AveIndirectTargets<<"\n";
	OP<<"# Number of indirect calls: \t\t\t"<<GCtx->IndirectCallInsts.size()<<"\n";   
	OP<<"# Number of indirect calls with targets: \t"<<GCtx->NumValidIndirectCalls<<"\n";
	OP<<"# Number of indirect-call targets: \t\t"<<GCtx->NumIndirectCallTargets<<"\n";
	OP<<"# Number of address-taken functions: \t\t"<<GCtx->AddressTakenFuncs.size()<<"\n";
	OP<<"# Number of multi-layer calls: \t\t\t"<<GCtx->NumSecondLayerTypeCalls<<"\n";
	OP<<"# Number of multi-layer targets: \t\t"<<GCtx->NumSecondLayerTargets<<"\n";  
	OP<<"# Number of one-layer calls: \t\t\t"<<GCtx->NumFirstLayerTypeCalls<<"\n";
	OP<<"# Number of one-layer targets: \t\t\t"<<GCtx->NumFirstLayerTargets<<"\n";

}

std::unordered_map<std::string, BBInfo> BBMapping;

void getBBMapping(GlobalContext *GCtx) {
	// 1. set up the mapping for each basic block, leave the successors blank for now
	OP << "\n\n############## Basic Block Mapping ##############\n";
	for(auto &M : GCtx->Modules) {
		Module *module = M.first;

		for (Function& func : *module) {
			#if DEBUG_MAPPING
			OP << "\n\nFunction: " << func.getName() << "\n";
			#endif
			if (func.getBasicBlockList().size() == 0) {
				#if DEBUG_MAPPING
				OP << "No basic block in function " << func.getName() << "\n";
				#endif
				continue;
			}

			// traverse each basic block
			for (BasicBlock& bb : func) {
				BBInfo info;
				info.name = func.getName().str() + "&" + bb.getName().str();
				info.path = "";
				for (Instruction& inst : bb) {
					MDNode *N = inst.getMetadata("dbg");
					if (N) {
						DILocation* Loc = cast<DILocation>(N);
						info.lines.insert(Loc->getLine());
						if (info.path == "") {
							// remove the leading "./" in the path
							std::string path = Loc->getFilename().str();
							if (path[0] == '.' && path[1] == '/') {
								path = path.substr(2);
							}
							info.path = Loc->getDirectory().str() + "/" + path;
						}
					}
				}
				BBMapping[info.name] = info;
			}
		}
	}
	#if DEBUG_MAPPING
	OP << "Basic Block Mapping set up complete.\n";
	#endif

	// 2. set up the successors for each basic block
	#if DEBUG_MAPPING
	OP << "\n\n############## Basic Block Successors ##############\n";
	#endif
	for(auto &M : GCtx->Modules) {
		Module *module = M.first;
		for (Function& func : *module) {
			#if DEBUG_MAPPING
			OP << "\n\nFunction: " << func.getName() << "\n";
			#endif
			if (func.getBasicBlockList().size() == 0) {
				#if DEBUG_MAPPING
				OP << "No basic block in function " << func.getName() << "\n";
				#endif
				continue;
			}

			for (BasicBlock& bb : func) {
				BBInfo& info = BBMapping[func.getName().str() + "&" + bb.getName().str()];
				// 2.1 intra-function basic block successors
				for (BasicBlock* succ : successors(&bb)) {
					info.successors.insert(func.getName().str() + "&" + succ->getName().str());
				}
				#if DEBUG_MAPPING
				OP << "intra-function basic block successors set up complete.\n";
				#endif
				// 2.2 inter-function basic block successors
				for (Instruction& inst : bb) {
					if (CallInst *callInst = dyn_cast<CallInst>(&inst)) {
						if (GCtx->Callees.find(callInst) != GCtx->Callees.end()) {
							#if DEBUG_INDIRECT_MAPPING
							// check if the current call instruction is an indirect call
							if (callInst->isIndirectCall()) {
								OP << "Indirect Call: " << *callInst << "\n";
							}
							#endif
							for (Function* callee : GCtx->Callees[callInst]) {
								// check if the callee has an entry block
								if (callee->getBasicBlockList().size() == 0) {
									continue;
								}

								BasicBlock& entry = callee->getEntryBlock();
								info.successors.insert(callee->getName().str() + "&" + entry.getName().str());

								#if DEBUG_INDIRECT_MAPPING
								OP << "\t" << callee->getName().str() + "&" + entry.getName().str() << "\n";
								#endif
							}
						}
					}
				}
				#if DEBUG_MAPPING
				OP << "inter-function basic block successors set up complete.\n";
				#endif
			}
		}
	}
	#if DEBUG_MAPPING
	OP << "Basic Block Successors set up complete.\n";
	#endif
	// 3. save the mapping as a JSON file
	#if DEBUG_MAPPING
	OP << "\n\n############## Save Mapping as a JSON file ##############\n";
	#endif
	std::ofstream outFile("BBMapping.json");
	writeMappingToJson(outFile, BBMapping);
	OP << "Basic Block Mapping saved as BBMapping.json.\n";
}

int main(int argc, char **argv) {

	// Print a stack trace if we signal out.
	sys::PrintStackTraceOnErrorSignal(argv[0]);
	PrettyStackTraceProgram X(argc, argv);

	llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

	cl::ParseCommandLineOptions(argc, argv, "global analysis\n");
	SMDiagnostic Err;

	// Loading modules
	OP << "Total " << InputFilenames.size() << " file(s)\n";

	for (unsigned i = 0; i < InputFilenames.size(); ++i) {

		LLVMContext *LLVMCtx = new LLVMContext();
		std::unique_ptr<Module> M = parseIRFile(InputFilenames[i], Err, *LLVMCtx);

		if (M == NULL) {
			OP << argv[0] << ": error loading file '"
				<< InputFilenames[i] << "'\n";
			continue;
		}

		Module *Module = M.release();
		StringRef MName = StringRef(strdup(InputFilenames[i].data()));
		GlobalCtx.Modules.push_back(std::make_pair(Module, MName));
		GlobalCtx.ModuleMaps[Module] = InputFilenames[i];
	}

	//
	// Main workflow
	//

	ENABLE_MLTA = MLTA;

	// Build global callgraph.
	CallGraphPass CGPass(&GlobalCtx);
	CGPass.run(GlobalCtx.Modules);

	// Print final results
	PrintResults(&GlobalCtx);

	getBBMapping(&GlobalCtx);

	return 0;
}


#include "core/kernel.h"
#include "cuda/cuda_runtime.h"
#include "ffi/ffi_embed.h"
#include "nnet/Visitor/AsTVMVisitor.h"
#include "nvrtc.h"
#include "operators/membound.h"
#include "operators/pooling.h"

namespace py = pybind11;

namespace infini {

class TVMRecordObj : public PerfRecordObj {
    // TODO: Add more attrs
  public:
    size_t logSize, ptxSize;
    std::string log, ptx;
    std::vector<int> invokeParams;
    std::string kernelName;
};

using TVMRecord = Ref<TVMRecordObj>;

class MemboundTVM : public Kernel {
  public:
    void compute(const Operator &_op, const PerfRecord &record,
                 const RuntimeObj *_context) const override {
        auto op = as<MemBoundObj>(_op);
        // auto context = dynamic_cast<const CudaRuntimeObj *>(_context);
        auto tvmRecord = std::dynamic_pointer_cast<TVMRecordObj>(record);

        // prepare for evaluation
        CUmodule module;
        CUfunction kernel;
        checkCUresult(cuModuleLoadDataEx(&module, tvmRecord->ptx.data(), 0,
                                         nullptr, nullptr));
        checkCUresult(cuModuleGetFunction(&kernel, module,
                                          tvmRecord->kernelName.c_str()));
        std::vector<void *> args;
        for (auto &&in : op->getInputs()) {
            args.push_back(in->getRawDataPtr<void *>());
        }
        args.push_back(op->getOutput()->getRawDataPtr<void *>());
        std::vector<void *> argsPtr;
        for (auto &arg : args) {
            argsPtr.push_back(&arg);
        }
        auto invokeParams = tvmRecord->invokeParams;

        // begin evaluation
        cuLaunchKernel(kernel, invokeParams[0], invokeParams[1],
                       invokeParams[2], invokeParams[3], invokeParams[4],
                       invokeParams[5], 0, NULL, argsPtr.data(), 0);

        // free module
        checkCUresult(cuModuleUnload(module));
    }

    void compute(const Operator &_op,
                 const RuntimeObj *_context) const override {
        IT_ASSERT(false, "A TVM record is required for membound kernel.");
    }

    std::string getVarName(const Tensor &t) const {
        return "var_" + std::to_string(t->getGuid());
    }

    // Premise: op is idempotent since it is called multiple times.
    PerfRecord tune(const Operator &_op,
                    const RuntimeObj *_context) const override {
        TVMRecord ret = std::make_shared<TVMRecordObj>();
        auto op = as<MemBoundObj>(_op);
        auto context = dynamic_cast<const CudaRuntimeObj *>(_context);

        // invoke Ansor to tune a membound kernel
        std::string func = "mem_bound_" + std::to_string(op->getGuid());
        std::string kernelName = func + "_kernel0";
        nnet::AsTVMVisitor visitor;
        visitor.dispatch(op->getNnetExpr());
        auto &&stmts = visitor.getStmts();
        auto &&inShapes = visitor.getInputShapes();
        auto &&outShape = visitor.getOutputShape();

        std::vector<std::string> inputs;
        for (auto &&in : op->getInputs()) {
            inputs.emplace_back(getVarName(in));
        }
        std::string output = getVarName(op->getOutput());
        auto res = getAnsorCode(
            inShapes, std::vector<std::string>(inShapes.size(), "float32"),
            outShape, "float32", stmts, func, inputs, output);

        // compile the kernel
        auto funcCode = res.first;
        auto invokeParams = res.second;
        std::string fileName = func + ".cu";
        nvrtcProgram prog;
        nvrtcCreateProgram(&prog,            // prog
                           funcCode.c_str(), // buffer
                           fileName.c_str(), // name
                           0,                // numHeaders
                           NULL,             // headers
                           NULL);            // includeNames
        const char *opts[] = {"--gpu-architecture=compute_80", "--fmad=false"};
        nvrtcCompileProgram(prog,  // prog
                            2,     // numOptions
                            opts); // options

        // copy ptx and log to ret
        size_t logSize;
        nvrtcGetProgramLogSize(prog, &logSize);
        size_t ptxSize;
        nvrtcGetPTXSize(prog, &ptxSize);
        ret->logSize = logSize;
        ret->ptxSize = ptxSize;
        ret->log = std::string(logSize, ' ');
        ret->ptx = std::string(ptxSize, ' ');
        nvrtcGetProgramLog(prog, ret->log.data());
        nvrtcGetPTX(prog, ret->ptx.data());
        ret->invokeParams = invokeParams;
        ret->kernelName = kernelName;

        // prepare for evaluation
        CUmodule module;
        CUfunction kernel;
        checkCUresult(
            cuModuleLoadDataEx(&module, ret->ptx.data(), 0, nullptr, nullptr));
        checkCUresult(cuModuleGetFunction(&kernel, module, kernelName.c_str()));
        std::vector<void *> args;
        for (auto &&in : op->getInputs()) {
            args.push_back(in->getRawDataPtr<void *>());
        }
        args.push_back(op->getOutput()->getRawDataPtr<void *>());
        std::vector<void *> argsPtr;
        for (auto &arg : args) {
            argsPtr.push_back(&arg);
        }

        // Evaluate the kernel
        ret->time = timeit(
            [&]() {
                // TODO: run the kernel
                cuLaunchKernel(kernel, invokeParams[0], invokeParams[1],
                               invokeParams[2], invokeParams[3],
                               invokeParams[4], invokeParams[5], 0, NULL,
                               argsPtr.data(), 0);
            },
            [&]() { context->sync(); });

        // free module
        checkCUresult(cuModuleUnload(module));
        nvrtcDestroyProgram(&prog);

        return std::dynamic_pointer_cast<PerfRecordObj>(ret);
    }

    std::pair<std::string, std::vector<int>>
    getAnsorCode(const std::vector<std::vector<int>> &inDims,
                 const std::vector<std::string> &inDTypes,
                 const std::vector<int> &outDims, const std::string &outDType,
                 const std::string &lambda, const std::string &funcName,
                 const std::vector<std::string> &inputNames,
                 const std::string &outputName) const {
        std::string funcCode;
        std::vector<int> invokeParams;
        try {
            start_interpreter();
            auto func = py::module::import("cpp_plugin").attr("gen_ansor_op");
            py::tuple code = func(inDims, inDTypes, outDims, outDType, lambda,
                                  funcName, inputNames, outputName);
            funcCode = py::str(code[0]);
            auto temp = py::list(code[3]);
            for (int i = 0; i < 6; ++i) {
                invokeParams.push_back(temp[i].cast<int>());
            }
        } catch (py::error_already_set &e) {
            if (e.matches(PyExc_ImportError)) {
                std::cerr << "Import Error. Don't forget to set environment "
                             "variable PYTHONPATH to contain "
                             "<repo-root>/python"
                          << std::endl;
            }
            throw;
        }
        return std::make_pair(funcCode, invokeParams);
    }
};

REGISTER_KERNEL(Device::CUDA, OpType::MemBound, DataType::Float32, MemboundTVM,
                "Memobund_TVM_Ansor");
}; // namespace infini
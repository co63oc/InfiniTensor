﻿#include "core/graph_handler.h"
#include "operators/batch_norm.h"
#include "operators/concat.h"
#include "operators/element_wise.h"
#include "operators/gather.h"
#include "operators/matmul.h"
#include "operators/pooling.h"
#include "operators/reduce_mean.h"
#include "operators/reshape.h"
#include "operators/unary.h"

namespace infini {

static DataType dtype_repr_convert(int);

Tensor GraphHandlerObj::tensor(Shape dims, int dtype) {
    return g->addTensor(std::move(dims), dtype_repr_convert(dtype));
}

Tensor GraphHandlerObj::matmul(Tensor a, Tensor b, Tensor y, bool transA,
                               bool transB, Tensor bias, ActType act) {
    if (y) {
        g->addOpWithOutputs<MatmulObj>(std::move(a), std::move(b), y, transA,
                                       transB, std::move(bias), act);
        return y;
    } else {
        return g
            ->addOp<MatmulObj>(std::move(a), std::move(b), y, transA, transB,
                               std::move(bias), act)
            ->getOutput();
    }
}

Tensor GraphHandlerObj::batchNorm(Tensor input, Tensor output, Tensor mean,
                                  Tensor var, Tensor scale, Tensor bias,
                                  float momentum, float eps, bool training) {
    if (output) {
        g->addOpWithOutputs<BatchNormObj>(
            std::move(input), output, std::move(mean), std::move(var),
            std::move(scale), std::move(bias), momentum, eps, training);
        return output;
    } else {
        return g
            ->addOp<BatchNormObj>(std::move(input), output, std::move(mean),
                                  std::move(var), std::move(scale),
                                  std::move(bias), momentum, eps, training)
            ->getOutput();
    }
}

Tensor GraphHandlerObj::maxPool(Tensor input, Tensor output, int kh, int kw,
                                int dh, int dw, int ph, int pw, int sh,
                                int sw) {
    if (output) {
        g->addOpWithOutputs<MaxPoolObj>(std::move(input), output, kh, kw, dh,
                                        dw, ph, pw, sh, sw);
        return output;
    } else {
        return g
            ->addOp<MaxPoolObj>(std::move(input), output, kh, kw, dh, dw, ph,
                                pw, sh, sw)
            ->getOutput();
    }
}
Tensor GraphHandlerObj::avgPool(Tensor input, Tensor output, int kh, int kw,
                                int dh, int dw, int ph, int pw, int sh,
                                int sw) {
    if (output) {
        g->addOpWithOutputs<AvgPoolObj>(std::move(input), output, kh, kw, dh,
                                        dw, ph, pw, sh, sw);
        return output;
    } else {
        return g
            ->addOp<AvgPoolObj>(std::move(input), output, kh, kw, dh, dw, ph,
                                pw, sh, sw)
            ->getOutput();
    }
}

// see operators/element_wise.h
#define DEFINE_ELEMENT_WISE_METHOD(name, obj)                                  \
    Tensor GraphHandlerObj::name(Tensor a, Tensor b, Tensor c) {               \
        if (c) {                                                               \
            g->addOpWithOutputs<obj##Obj>(std::move(a), std::move(b), c);      \
            return c;                                                          \
        } else {                                                               \
            return g->addOp<obj##Obj>(std::move(a), std::move(b), c)           \
                ->getOutput();                                                 \
        }                                                                      \
    }

DEFINE_ELEMENT_WISE_METHOD(add, Add)
DEFINE_ELEMENT_WISE_METHOD(sub, Sub)
DEFINE_ELEMENT_WISE_METHOD(mul, Mul)
DEFINE_ELEMENT_WISE_METHOD(div, Div)
DEFINE_ELEMENT_WISE_METHOD(pow, Pow)

// see operators/unary.h
#define DEFINE_UNARY_METHOD(name, obj)                                         \
    Tensor GraphHandlerObj::name(Tensor x, Tensor y) {                         \
        if (y) {                                                               \
            g->addOpWithOutputs<obj##Obj>(std::move(x), y);                    \
            return y;                                                          \
        } else {                                                               \
            return g->addOp<obj##Obj>(std::move(x), y)->getOutput();           \
        }                                                                      \
    }

DEFINE_UNARY_METHOD(relu, Relu)
DEFINE_UNARY_METHOD(sigmoid, Sigmoid)
DEFINE_UNARY_METHOD(tanh, Tanh)
DEFINE_UNARY_METHOD(softmax, Softmax)
DEFINE_UNARY_METHOD(abs, Abs)
// see operators/reshape.h
DEFINE_UNARY_METHOD(identity, Identity)
DEFINE_UNARY_METHOD(flatten, Flatten)

Tensor GraphHandlerObj::reshape(Tensor data, Tensor reshaped, Shape shape) {
    if (reshaped) {
        g->addOpWithOutputs<ReshapeObj>(std::move(data), reshaped,
                                        std::move(shape));
        return reshaped;
    } else {
        return g->addOp<ReshapeObj>(std::move(data), reshaped, std::move(shape))
            ->getOutput();
    }
}

Tensor GraphHandlerObj::concat(TensorVec inputs, Tensor output, int dim) {
    if (output) {
        g->addOpWithOutputs<ConcatObj>(std::move(inputs), output, dim);
        return output;
    } else {
        return g->addOp<ConcatObj>(std::move(inputs), output, dim)->getOutput();
    }
}

Tensor GraphHandlerObj::gather(Tensor data, Tensor indices, Tensor output,
                               int axis) {
    if (output) {
        g->addOpWithOutputs<GatherObj>(std::move(data), std::move(indices),
                                       output, axis);
        return output;
    } else {
        return g
            ->addOp<GatherObj>(std::move(data), std::move(indices), output,
                               axis)
            ->getOutput();
    }
}

Tensor GraphHandlerObj::reduceMean(Tensor data, Tensor reduced,
                                   const optional<vector<int>> &axes,
                                   bool keepdims) {
    if (reduced) {
        g->addOpWithOutputs<ReduceMeanObj>(std::move(data), reduced, axes,
                                           keepdims);
        return reduced;
    } else {
        return g->addOp<ReduceMeanObj>(std::move(data), reduced, axes, keepdims)
            ->getOutput();
    }
}

static DataType dtype_repr_convert(int dtype) {
    switch ((OnnxDType)dtype) {
    case OnnxDType::FLOAT:
        return DataType::Float32;
    case OnnxDType::UINT32:
        return DataType::UInt32;
    case OnnxDType::UINT8:
        return DataType::UInt8;
    case OnnxDType::INT8:
        return DataType::Int8;
    case OnnxDType::UINT16:
        return DataType::UInt16;
    case OnnxDType::INT16:
        return DataType::Int16;
    case OnnxDType::INT32:
        return DataType::Int32;
    case OnnxDType::INT64:
        return DataType::Int64;
    default:
        IT_ASSERT(false, "Unsupported data type");
    }
}

} // namespace infini
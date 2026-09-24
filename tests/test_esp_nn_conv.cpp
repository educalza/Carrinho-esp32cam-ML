// Compares vendored ESP-NN against the installed TFLM reference, bit for bit.
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include "../firmware/esp32cam_autonomous/optimized_conv.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/conv.h"
extern "C" {
#include "../firmware/esp32cam_autonomous/src/esp_nn/esp_nn_ansi_headers.h"
}

// Minimal host context: exercise the real adapter, while fallback is a sentinel.
static TfLiteEvalTensor tensors[4];
static unsigned fallbackCalls=0;
void MicroPrintf(const char*, ...) {}
void ReportError(TfLiteContext*,const char*,...) {}
namespace tflite {
const int kConvInputTensor=0, kConvWeightsTensor=1, kConvBiasTensor=2, kConvOutputTensor=0;
TFLMRegistration Register_CONV_2D() {
    TFLMRegistration r={};
    r.invoke=[](TfLiteContext*,TfLiteNode*) { ++fallbackCalls; return kTfLiteOk; };
    return r;
}
namespace micro {
const TfLiteEvalTensor* GetEvalInput(const TfLiteContext*,const TfLiteNode* node,int index) {
    return &tensors[node->inputs->data[index]];
}
TfLiteEvalTensor* GetEvalOutput(const TfLiteContext*,const TfLiteNode* node,int index) {
    return &tensors[node->outputs->data[index]];
}
}
}

struct Array4 { int size; int data[4]; };

int main() {
    std::mt19937 random(713);
    unsigned cases=0;
    // First three match current model geometry; others cover boundaries and channel tails.
    const int layouts[][6]={{96,96,1,4,5,4},{24,24,4,8,3,2},{12,12,8,12,3,2},
        {7,9,3,5,3,1},{5,7,5,3,1,2},{4,6,1,1,5,1}};
    for(const auto& layout:layouts) for(int padding=0;padding<=1;++padding) {
        const int w=layout[0],h=layout[1],ci=layout[2],co=layout[3],k=layout[4],s=layout[5];
        // SAME may pad only the right/bottom edge (current model strides 4 and 2).
        const int ow=padding ? (w+s-1)/s : (w-k)/s+1;
        const int oh=padding ? (h+s-1)/s : (h-k)/s+1;
        const int px=padding ? std::max(0,(ow-1)*s+k-w)/2 : 0;
        const int py=padding ? std::max(0,(oh-1)*s+k-h)/2 : 0;
        if(ow<=0 || oh<=0) continue;
        data_dims_t in={w,h,ci,1},filter={k,k,ci,co},out={ow,oh,co,1};
        const int32_t inputShape[]={1,h,w,ci},filterShape[]={co,k,k,ci},outputShape[]={1,oh,ow,co};
        tflite::RuntimeShape is(4,inputShape),fs(4,filterShape),os(4,outputShape),bs(1,&co);
        for(int trial=0;trial<30;++trial) {
            std::vector<int8_t> input(w*h*ci),weights(k*k*ci*co),expected(ow*oh*co),actual(expected.size());
            std::vector<int32_t> bias(co),mult(co),shift(co);
            for(auto& x:input) x=trial==0 ? -128 : trial==1 ? 127 : int(random()%256)-128;
            for(auto& x:weights) x=int(random()%255)-127;
            for(int c=0;c<co;++c) {
                bias[c]=int(random()%20001)-10000;
                mult[c]=1073741824+random()%1073741823;
                shift[c]=-int(random()%15);
            }
            conv_params_t p={int(random()%256)-127,int(random()%256)-128,{s,s},{px,py},{1,1},
                {trial%2 ? 0 : -128,127}};
            quant_data_t q={shift.data(),mult.data()};
            tflite::ConvParams ref={};
            ref.input_offset=p.in_offset; ref.output_offset=p.out_offset;
            ref.stride_width=ref.stride_height=s;
            ref.dilation_width_factor=ref.dilation_height_factor=1;
            ref.padding_values.width=px; ref.padding_values.height=py;
            ref.quantized_activation_min=p.activation.min; ref.quantized_activation_max=p.activation.max;
            const auto* b=trial%3 ? bias.data() : nullptr;
            tflite::reference_integer_ops::ConvPerChannel(ref,mult.data(),shift.data(),
                is,input.data(),fs,weights.data(),bs,b,os,expected.data());
            esp_nn_conv_s8_opt(&in,input.data(),&filter,weights.data(),b,&out,actual.data(),&p,&q);
            assert(expected==actual);
            Array4 dims[4]={{4,{1,h,w,ci}},{4,{co,k,k,ci}},{1,{co,0,0,0}},{4,{1,oh,ow,co}}};
            void* buffers[]={input.data(),weights.data(),bias.data(),actual.data()};
            for(int i=0;i<4;++i) {
                tensors[i]={};
                tensors[i].dims=reinterpret_cast<TfLiteIntArray*>(&dims[i]);
                tensors[i].type=i==2 ? kTfLiteInt32 : kTfLiteInt8;
                tensors[i].data.raw=static_cast<char*>(buffers[i]);
            }
            Array4 ins={b ? 3 : 2,{0,1,2,0}},outs={1,{3,0,0,0}};
            TfLiteConvParams params={};
            params.stride_width=params.stride_height=s;
            params.dilation_width_factor=params.dilation_height_factor=1;
            tflite::OpDataConv data={};
            data.padding.width=px; data.padding.height=py;
            data.input_zero_point=-p.in_offset; data.output_zero_point=p.out_offset;
            data.per_channel_output_multiplier=mult.data(); data.per_channel_output_shift=shift.data();
            data.output_activation_min=p.activation.min; data.output_activation_max=p.activation.max;
            TfLiteNode node={};
            node.inputs=reinterpret_cast<TfLiteIntArray*>(&ins); node.outputs=reinterpret_cast<TfLiteIntArray*>(&outs);
            node.builtin_data=&params; node.user_data=&data;
            TfLiteContext context={};
            context.ReportError=ReportError;
            std::fill(actual.begin(),actual.end(),42);
            auto adapter=car_ml::RegisterOptimizedConv();
            assert(adapter.invoke(&context,&node)==kTfLiteOk && expected==actual);
            // Unsupported dilation must call reference rather than silently ignoring it.
            const unsigned before=fallbackCalls;
            params.dilation_width_factor=2;
            assert(adapter.invoke(&context,&node)==kTfLiteOk && fallbackCalls==before+1);
            params.dilation_width_factor=1;
            if(trial==0) {
                // Reference sentinel keeps these deliberately different bytes: verification must fail.
                for(size_t i=0;i<actual.size();++i) actual[i]=expected[i]==127 ? -128 : expected[i]+1;
                std::vector<uint8_t> scratch(actual.size());
                car_ml::SetConvVerification(scratch.data(),scratch.size());
                assert(adapter.invoke(&context,&node)==kTfLiteError);
                assert(adapter.invoke(&context,&node)==kTfLiteOk); // Actual output now matches.
                car_ml::SetConvVerification(nullptr,0);
            }
            ++cases;
        }
    }
    std::printf("ESP-NN: %u convolutions and adapter outputs bit-exact; fallback and mismatch interlock passed\n",cases);
}

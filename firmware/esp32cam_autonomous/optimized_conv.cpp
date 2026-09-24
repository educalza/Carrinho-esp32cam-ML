#include "optimized_conv.h"
#include <string.h>
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_log.h"
extern "C" {
#include "src/esp_nn/esp_nn_ansi_headers.h"
}

namespace car_ml {
namespace {
uint8_t* verification=nullptr;
size_t verificationSize=0;
unsigned optimizedCalls=0, referenceCalls=0;

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
    using namespace tflite;
    const auto* input=micro::GetEvalInput(context,node,kConvInputTensor);
    const auto* filter=micro::GetEvalInput(context,node,kConvWeightsTensor);
    const auto* bias=NumInputs(node)==3 ? micro::GetEvalInput(context,node,kConvBiasTensor) : nullptr;
    auto* output=micro::GetEvalOutput(context,node,kConvOutputTensor);
    const auto& params=*static_cast<const TfLiteConvParams*>(node->builtin_data);
    const auto& data=*static_cast<const OpDataConv*>(node->user_data);
    const auto reference=Register_CONV_2D();
    // ESP32 generic optimized kernel has no dilation/batch/group support.
    // Its 1x1 fast path also assumes no padding. Keep reference for other models.
    bool supported=input->type==kTfLiteInt8 && filter->type==kTfLiteInt8 &&
        output->type==kTfLiteInt8 && (!bias || bias->type==kTfLiteInt32) &&
        input->dims->size==4 && filter->dims->size==4 && output->dims->size==4 &&
        input->dims->data[0]==1 && output->dims->data[0]==1 &&
        params.dilation_width_factor==1 && params.dilation_height_factor==1 &&
        input->dims->data[3]==filter->dims->data[3] && data.filter_zero_point==0;
    if (supported) {
        for(int i=1;i<4;++i)
            supported=supported && input->dims->data[i]>0 && input->dims->data[i]<=65535 &&
                filter->dims->data[i]>0 && filter->dims->data[i]<=65535 &&
                output->dims->data[i]>0 && output->dims->data[i]<=65535;
        if(filter->dims->data[1]==1 && filter->dims->data[2]==1)
            supported=supported && data.padding.width==0 && data.padding.height==0;
    }
    if(!supported) { ++referenceCalls; return reference.invoke(context,node); }
    const data_dims_t in={input->dims->data[2],input->dims->data[1],input->dims->data[3],1};
    const data_dims_t weights={filter->dims->data[2],filter->dims->data[1],filter->dims->data[3],filter->dims->data[0]};
    const data_dims_t out={output->dims->data[2],output->dims->data[1],output->dims->data[3],1};
    const conv_params_t conv={-data.input_zero_point,data.output_zero_point,
        {params.stride_width,params.stride_height},{data.padding.width,data.padding.height},
        {1,1},{data.output_activation_min,data.output_activation_max}};
    const quant_data_t quant={data.per_channel_output_shift,data.per_channel_output_multiplier};
    int8_t* result=micro::GetTensorData<int8_t>(output);
    const size_t bytes=size_t(out.width)*out.height*out.channels;
    if(verification) {
        TF_LITE_ENSURE(context,bytes<=verificationSize);
        TF_LITE_ENSURE_STATUS(reference.invoke(context,node));
        memcpy(verification,result,bytes);
    }
    esp_nn_conv_s8_opt(&in,micro::GetTensorData<int8_t>(input),
        &weights,micro::GetTensorData<int8_t>(filter),micro::GetOptionalTensorData<int32_t>(bias),
        &out,result,&conv,&quant);
    ++optimizedCalls;
    if(verification && memcmp(verification,result,bytes)!=0) {
        MicroPrintf("[ML] ESP-NN divergiu da convolucao de referencia; movimento bloqueado");
        return kTfLiteError;
    }
    return kTfLiteOk;
}
}
TFLMRegistration RegisterOptimizedConv() {
    auto registration=tflite::Register_CONV_2D();
    registration.invoke=Eval; // Preserve reference preparation and quantization arithmetic.
    return registration;
}
void SetConvVerification(uint8_t* scratch,size_t size) { verification=scratch; verificationSize=size; }
unsigned OptimizedConvCalls() { return optimizedCalls; }
unsigned ReferenceConvCalls() { return referenceCalls; }
}

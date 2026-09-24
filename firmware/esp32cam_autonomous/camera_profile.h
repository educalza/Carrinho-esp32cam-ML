#pragma once

namespace car_camera {
// OV2640 get_reg/set_reg encode bank in bit 8; do not write BANK_SEL manually.
constexpr int kOv2640Pid=0x26;
constexpr int kSensorClkrc=0x111;
constexpr int kDividerMask=0x3f;
struct ClockResult {
    bool ok;
    bool fast;
    int original;
    int effective;
    const char* reason;
};

template<typename Sensor>
ClockResult configureClock(Sensor* sensor, bool fast) {
    if (!sensor || sensor->id.PID!=kOv2640Pid || !sensor->get_reg || !sensor->set_reg)
        return {true,false,-1,-1,"sensor_sem_perfil"};
    const int original=sensor->get_reg(sensor,kSensorClkrc,0xff);
    if (original<0 || original>255) return {true,false,original,-1,"leitura_indisponivel"};
    if (!fast) return {true,false,original,original,"padrao_solicitado"};
    // Only accelerate the known CIF/QVGA grayscale baseline, with doubler disabled.
    // Value 3 divides by four; value 1 divides by two. Preserve all other bits.
    if ((original & kDividerMask)!=3 || (original & 0x80))
        return {true,false,original,original,"clock_original_diferente"};
    const int expected=(original & ~kDividerMask)|1;
    if (sensor->set_reg(sensor,kSensorClkrc,kDividerMask,1)==0 &&
        sensor->get_reg(sensor,kSensorClkrc,0xff)==expected)
        return {true,true,original,expected,"aplicado"};
    const bool restored=sensor->set_reg(sensor,kSensorClkrc,0xff,original)==0 &&
        sensor->get_reg(sensor,kSensorClkrc,0xff)==original;
    return {restored,false,original,restored ? original : -1,
        restored ? "falha_rapido_padrao_restaurado" : "falha_ao_restaurar_clock"};
}
}

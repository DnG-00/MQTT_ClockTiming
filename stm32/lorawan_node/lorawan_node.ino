// The STM32WL as a LoRaWAN test device for the testbed (docs/testbed.md step 9): no sensors, an uplink every NODE_S
// seconds on FPort 1 ({"n":<count>}), then Home Assistant's answer ("ack <FCnt>") in RX1 or RX2. ABP (no join: DevAddr
// and session keys built in), LoRaWAN 1.0.3, AES-128: ST's LoRaWAN stack as STM32duino's STM32LoRaWAN library has it,
// AS923-1. Built and flashed by stm32/run_node.sh; node_secrets.h (DevAddr, NwkSKey, AppSKey as hex) is made from the
// provisioning file for that build only.
//
// Serial, 115200 (PA2 / PA3: on the LM402 kit, its ST-Link's serial port):
//   #device ...                                   once at start
//   #up <n> FCnt <f> DR<d>: <ms> ms               an uplink and its receive windows done, <ms> from the send
//   #down <n> FCntDown <f> port <p> RSSI <r> SNR <s>: <text>   the answer to uplink <n>; "#down <n> none" if none
//   #count up <u> down <d>                        after each uplink
//   cyc,uplink,fcnt,...                           once at start: the header of the rows below (CYC_HDR)
//   cyc,<n>,<FCnt>,<DR>,<ok|failed>,<ms>,<1|0>,<MHz>,<cycles>,<calls> x 4   after each uplink: CPU cycles in the
//                                                 library's LoRaWAN crypto (below): the uplink's MIC and AES, then the
//                                                 downlink's (0,0 if none came); stm32/cyc_csv.py turns the rows into
//                                                 results/stm32_<device>_cyc.csv (run_node.sh log does, as they come)
//
// Cycles: DWT->CYCCNT, the Cortex-M4's cycle counter (every core clock; the RP2040's M0+ has none). The crypto is inside
// ST's library: run_node.sh links with -Wl,--wrap=<f> for the three secure-element calls below, so LoRaMacCrypto.c's
// calls to them come to __wrap_<f> here, which counts around __real_<f> (soft-se.c, unchanged). mic: the uplink's MIC
// (SecureElementComputeAesCmac), the downlink's check (SecureElementVerifyAesCmac); aes: SecureElementAesEncrypt, one
// call per 16-byte block of FRMPayload (counter mode). A downlink's processing starts with its MIC check, so the AES
// calls after it are the downlink's. Calls inside soft-se.c itself (key derivation) aren't counted: --wrap only takes
// calls between object files (and LTO would undo it: the core's default build has none).
#include <STM32LoRaWAN.h>
#include "STM32CubeWL/LoRaWAN/Mac/secure-element.h"
#include "node_secrets.h"

struct se_count { uint32_t cyc, n; };
static se_count up_mic, up_aes, dn_mic, dn_aes;
static bool in_down;  // set by the downlink's MIC check, cleared before each uplink

static inline void se_add(se_count &s, uint32_t c0) { s.cyc += DWT->CYCCNT - c0; s.n++; }

extern "C" {
SecureElementStatus_t __real_SecureElementComputeAesCmac(uint8_t *micBxBuffer, uint8_t *buffer, uint32_t size,
                                                         KeyIdentifier_t keyID, uint32_t *cmac);
SecureElementStatus_t __real_SecureElementVerifyAesCmac(uint8_t *buffer, uint32_t size, uint32_t expectedCmac,
                                                        KeyIdentifier_t keyID);
SecureElementStatus_t __real_SecureElementAesEncrypt(uint8_t *buffer, uint32_t size, KeyIdentifier_t keyID,
                                                     uint8_t *encBuffer);

SecureElementStatus_t __wrap_SecureElementComputeAesCmac(uint8_t *micBxBuffer, uint8_t *buffer, uint32_t size,
                                                         KeyIdentifier_t keyID, uint32_t *cmac) {
    uint32_t c0 = DWT->CYCCNT;
    SecureElementStatus_t r = __real_SecureElementComputeAesCmac(micBxBuffer, buffer, size, keyID, cmac);
    se_add(in_down ? dn_mic : up_mic, c0);
    return r;
}
SecureElementStatus_t __wrap_SecureElementVerifyAesCmac(uint8_t *buffer, uint32_t size, uint32_t expectedCmac,
                                                        KeyIdentifier_t keyID) {
    in_down = true;
    uint32_t c0 = DWT->CYCCNT;
    SecureElementStatus_t r = __real_SecureElementVerifyAesCmac(buffer, size, expectedCmac, keyID);
    se_add(dn_mic, c0);
    return r;
}
SecureElementStatus_t __wrap_SecureElementAesEncrypt(uint8_t *buffer, uint32_t size, KeyIdentifier_t keyID,
                                                     uint8_t *encBuffer) {
    uint32_t c0 = DWT->CYCCNT;
    SecureElementStatus_t r = __real_SecureElementAesEncrypt(buffer, size, keyID, encBuffer);
    se_add(in_down ? dn_aes : up_aes, c0);
    return r;
}
}

#ifndef NODE_S
#define NODE_S 60
#endif
#ifndef NODE_DR
#define NODE_DR 0  // SF12, as the Picos; AS923's 400 ms dwell limit (ST's default) holds it at DR2 (SF10)
#endif

// the per-uplink cycle rows: stm32/cyc_csv.py reads them by these names
#define CYC_HDR "cyc,uplink,fcnt,dr,status,up_ms,reply,cpu_mhz,up_mic_cyc,up_mic_calls,up_aes_cyc,up_aes_calls," \
                "down_mic_cyc,down_mic_calls,down_aes_cyc,down_aes_calls"

STM32LoRaWAN modem;
static uint32_t ups, downs, next_ms;

void setup() {
    Serial.begin(115200);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // the debug block on (needs no debugger attached), then
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;             // the cycle counter: running, never reset (differences wrap fine)
    Serial.println("#device ABP (no join: DevAddr and session keys built in), LoRaWAN 1.0.3, AES-128: STM32WL, "
                   "ST's stack (STM32LoRaWAN), AS923-1");
    Serial.printf("#cpu %lu MHz, cycles from DWT->CYCCNT\n", (unsigned long)(SystemCoreClock / 1000000u));
    if (!modem.begin(AS923)) {
        Serial.println("#error modem.begin(AS923)");
        while (true) {}
    }
    modem.setADR(false);  // the data rate stays where it is set
    if (!modem.dataRate(NODE_DR))
        Serial.printf("#dr DR%d refused (AS923's dwell limit): DR%d\n", NODE_DR, modem.getDataRate());
    modem.setPort(1);     // Home Assistant answers FPort 1 only
    if (!modem.joinABP(NODE_DEVADDR, NODE_NWKSKEY, NODE_APPSKEY)) {
        Serial.println("#error joinABP: DevAddr or a key not 8 / 32 hex digits");
        while (true) {}
    }
    // ponytail: FCnt starts at 0 at every reset (the library can't set it); ChirpStack's counters back to 0 on the
    // device's Activation tab after one. A counter kept in flash (MIB_NVM_CTXS) if resets get frequent.
    Serial.printf("#node every %d s: {\"n\":<count>} on FPort 1, DevAddr %s\n", NODE_S, NODE_DEVADDR);
    Serial.println(CYC_HDR);
    next_ms = millis();
}

void loop() {
    if ((int32_t)(millis() - next_ms) < 0)
        return;
    next_ms += NODE_S * 1000UL;
    char msg[16];  // 11 bytes at most at DR2 under the dwell limit: n up to 99999
    int len = snprintf(msg, sizeof msg, "{\"n\":%lu}", (unsigned long)++ups);
    modem.beginPacket();
    modem.write((const uint8_t *)msg, len);
    up_mic = up_aes = dn_mic = dn_aes = se_count{0, 0};  // this uplink's crypto, and its downlink's
    in_down = false;
    uint32_t t0 = millis();
    int sent = modem.endPacket();  // blocks: the uplink, then RX1 and (with nothing in RX1) RX2
    uint32_t ms = millis() - t0;
    if (sent < 0)
        Serial.printf("#up %lu failed after %lu ms\n", (unsigned long)ups, (unsigned long)ms);
    else
        Serial.printf("#up %lu FCnt %ld DR%d: %lu ms\n", (unsigned long)ups, (long)modem.getFCU(),
                      modem.getDataRate(), (unsigned long)ms);
    bool reply = modem.available();
    if (reply) {
        char rx[65];
        int n = modem.read((uint8_t *)rx, sizeof rx - 1);
        for (int i = 0; i < n; i++)
            if (rx[i] < 32 || rx[i] > 126) rx[i] = '.';
        rx[n] = 0;
        downs++;
        Serial.printf("#down %lu FCntDown %ld port %u RSSI %d SNR %d: %s\n", (unsigned long)ups, (long)modem.getFCD(),
                      modem.getDownlinkPort(), modem.getDownlinkRssi(), modem.getDownlinkSnr(), rx);
    } else {
        Serial.printf("#down %lu none\n", (unsigned long)ups);
    }
    Serial.printf("cyc,%lu,%ld,%d,%s,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)ups,
                  (long)modem.getFCU(), modem.getDataRate(), sent < 0 ? "failed" : "ok", (unsigned long)ms, reply,
                  (unsigned long)(SystemCoreClock / 1000000u), (unsigned long)up_mic.cyc, (unsigned long)up_mic.n,
                  (unsigned long)up_aes.cyc, (unsigned long)up_aes.n, (unsigned long)dn_mic.cyc,
                  (unsigned long)dn_mic.n, (unsigned long)dn_aes.cyc, (unsigned long)dn_aes.n);
    Serial.printf("#count up %lu down %lu\n", (unsigned long)ups, (unsigned long)downs);
}

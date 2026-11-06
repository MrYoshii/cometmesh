#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source patches for the fetched dependencies (idf/fetch-deps.sh runs this).

Each patch replaces text that must occur exactly once, so an upstream change fails
loudly instead of silently building something else. Keep patches few and explain each.

Usage: apply.py <idf/deps>
"""
import os
import sys

DEPS = sys.argv[1]


def sub(rel, pairs, why):
    path = os.path.join(DEPS, rel)
    s = open(path, encoding="utf-8").read()
    for old, new in pairs:
        if new in s:
            continue  # already applied
        n = s.count(old)
        if n != 1:
            sys.exit(f"!! patch '{why}': expected 1 match in {rel}, found {n}: {old[:70]!r}")
        s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
    print(f"patched: {why}")


# esp-nimble-cpp finds the Arduino component by its registry name. Without the component
# manager our arduino-esp32 is a plain local component of that name.
sub("idf/h2zero__esp-nimble-cpp/CMakeLists.txt", [
    ('if(__COMPONENT_TARGETS MATCHES "___idf_espressif__arduino-esp32")',
     'if(__COMPONENT_TARGETS MATCHES "___idf_arduino-esp32")'),
], "esp-nimble-cpp: local arduino-esp32 component")


# The core's BLE companion interface targets NimBLE-Arduino 1.4 (PlatformIO build); here it
# builds against esp-nimble-cpp 2.x, whose callbacks take NimBLEConnInfo. Same port as
# wadamesh's tanmatsu/fetch-deps.sh.
BH = "lib/meshcore/src/helpers/esp32/SerialBLEInterface.h"
BC = "lib/meshcore/src/helpers/esp32/SerialBLEInterface.cpp"
sub(BH, [
  ("""  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
  void onDisconnect(NimBLEServer* pServer) override;
  void onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) override;
  uint32_t onPassKeyRequest() override;
  bool onConfirmPIN(uint32_t pass_key) override;
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override;""",
   """  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override;
  uint32_t onPassKeyDisplay() override;
  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;"""),
  ("  void onWrite(NimBLECharacteristic* pCharacteristic) override;",
   "  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;"),
], "SerialBLEInterface.h: esp-nimble-cpp 2.x")
sub(BC, [
  ('#include "SerialBLEInterface.h"',
   '#include "SerialBLEInterface.h"\n#include <esp_mac.h>   // esp_efuse_mac_get_default (IDF 5.x moved it; Arduino S3 gets it transitively)'),
  ("""uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");""",
   """uint32_t SerialBLEInterface::onPassKeyDisplay() {
  BLE_DEBUG_PRINTLN("onPassKeyDisplay()");"""),
  ("""bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}""",
   """void SerialBLEInterface::onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPassKey(%u)", pass_key);
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}"""),
  ("""void SerialBLEInterface::onAuthenticationComplete(ble_gap_conn_desc* desc) {
  if (desc && desc->sec_state.encrypted) {""",
   """void SerialBLEInterface::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  if (connInfo.isEncrypted()) {"""),
  ("pServer->disconnect(desc ? desc->conn_handle : last_conn_id);",
   "pServer->disconnect(connInfo.getConnHandle());"),
  ("""void SerialBLEInterface::onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d", desc ? desc->conn_handle : -1);
  if (desc) {
    last_conn_id = desc->conn_handle;
    memcpy(_peer_bda, desc->peer_ota_addr.val, 6);   // NimBLE addr is little-endian
    _peer_bda_valid = true;
  }
}""",
   """void SerialBLEInterface::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d", connInfo.getConnHandle());
  last_conn_id = connInfo.getConnHandle();
  memcpy(_peer_bda, connInfo.getAddress().getVal(), 6);   // NimBLE addr is little-endian
  _peer_bda_valid = true;
}"""),
  ("void SerialBLEInterface::onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) {",
   "void SerialBLEInterface::onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {"),
  ("void SerialBLEInterface::onDisconnect(NimBLEServer* pServer) {",
   "void SerialBLEInterface::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {"),
  ("void SerialBLEInterface::onWrite(NimBLECharacteristic* pCharacteristic) {",
   "void SerialBLEInterface::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {"),
  ("adv->setScanResponse(true);", "adv->enableScanResponse(true);"),
], "SerialBLEInterface.cpp: esp-nimble-cpp 2.x")

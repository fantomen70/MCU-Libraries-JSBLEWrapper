#ifndef JSBLEWRAPPER_H
#define JSBLEWRAPPER_H

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>
#include <unordered_map>

#if defined(ESP32)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
#endif

class JSBLEWrapper
{
public:
  using CommandHandler = void (*)(JSBLEWrapper* self, const std::string& cmd, const std::string& value);

  JSBLEWrapper(const std::string& deviceName,
               const std::string& serviceUUID,
               const std::string& characteristicTx,
               const std::string& characteristicRx);

  void Start();
  void Stop();

  void SendData(const std::string& command, const std::string& value);

  void SetOnReceiveCallback(void (*onReceive)(std::string cmd, std::string value));
  void SetOnDisconnectedCallback(void (*onDisconnected)());

  void RegisterCommandHandler(const std::string& cmd, CommandHandler handler);
  void UnregisterCommandHandler(const std::string& cmd);
  void ClearCommandHandlers();

  std::string GetDeviceIdHex() const;

private:
  bool TryDispatchCommand(const std::string& cmd, const std::string& value);

  void BuildDeviceId();
  void StartAdvertising();

#if defined(ESP32)
  static void BleTaskEntry(void* parameter);
  void BleTaskLoop();
  void StartBleTaskIfNeeded();
  void StopBleTaskIfRunning();

  TaskHandle_t _bleTaskHandle = nullptr;
  volatile bool _runTask = false;
#endif

  bool deviceConnected = false;

  std::string _deviceName;
  std::string _serviceUUID;
  std::string _characteristicTxUUID;
  std::string _characteristicRxUUID;

  NimBLEServer*         _server = nullptr;
  NimBLEService*        _service = nullptr;
  NimBLECharacteristic* _txCharacteristic = nullptr;
  NimBLECharacteristic* _rxCharacteristic = nullptr;

  class JSBLECharacteristicCallbacks* _rxCallbacks = nullptr;
  class JSBLEServerCallbacks*         _serverCallbacks = nullptr;

  void (*_onReceiveData)(std::string, std::string) = nullptr;
  void (*_onDisconnectedCallback)() = nullptr;

  std::unordered_map<std::string, CommandHandler> _handlers;

  uint8_t _deviceId[8]{};

  friend class JSBLECharacteristicCallbacks;
  friend class JSBLEServerCallbacks;
};

class JSBLECharacteristicCallbacks : public NimBLECharacteristicCallbacks
{
public:
  explicit JSBLECharacteristicCallbacks(JSBLEWrapper* owner);
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;

private:
  JSBLEWrapper* _owner;
};

class JSBLEServerCallbacks : public NimBLEServerCallbacks
{
public:
  explicit JSBLEServerCallbacks(JSBLEWrapper* owner);
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;

private:
  JSBLEWrapper* _owner;
};

#endif // JSBLEWRAPPER_H

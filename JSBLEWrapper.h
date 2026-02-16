#ifndef JSBLEWRAPPER_H
#define JSBLEWRAPPER_H

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>
#include <unordered_map>

/**
 * @brief Enkel wrapper runt ESP32 BLE Server (NimBLE-Arduino 2.3.7)
 *
 * - Startar BLE-tjänst
 * - Tar emot AT-kommandon: "AT" + cmd(2 chars) + value
 * - Skickar notifieringar (TX notify)
 * - Tar emot data (RX write)
 * - Stöd för kommandodispach (registrera handler per cmd)
 * - Advertisar Manufacturer Data med stabilt DeviceId
 *
 * Ingen bakgrundstask behövs.
 */
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

  /// Payload: "AT" + command(2) + value
  void SendData(const std::string& command, const std::string& value);

  void SetOnReceiveCallback(void (*onReceive)(std::string cmd, std::string value));
  void SetOnDisconnectedCallback(void (*onDisconnected)());

  void RegisterCommandHandler(const std::string& cmd, CommandHandler handler);
  void UnregisterCommandHandler(const std::string& cmd);
  void ClearCommandHandlers();

  /// Debug/identifiering: samma 8 bytes som advertisas i manufacturer data (hex, 16 tecken)
  std::string GetDeviceIdHex() const;

private:
  bool TryDispatchCommand(const std::string& cmd, const std::string& value);

  void BuildDeviceId();
  void StartAdvertising();

  /// "BaseName-XXXX" där XXXX är sista 4 hex av DeviceId
  std::string BuildAdvertisedName() const;

  bool _deviceConnected = false;

  std::string _deviceName; // base name
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

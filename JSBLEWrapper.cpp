#include "JSBLEWrapper.h"

static std::string BytesToHex(const uint8_t* data, size_t len)
{
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++)
  {
    out.push_back(hex[(data[i] >> 4) & 0xF]);
    out.push_back(hex[data[i] & 0xF]);
  }
  return out;
}

JSBLEWrapper::JSBLEWrapper(const std::string& deviceName,
                           const std::string& serviceUUID,
                           const std::string& characteristicTx,
                           const std::string& characteristicRx)
  : _deviceName(deviceName),
    _serviceUUID(serviceUUID),
    _characteristicTxUUID(characteristicTx),
    _characteristicRxUUID(characteristicRx)
{
  BuildDeviceId();
}

void JSBLEWrapper::BuildDeviceId()
{
  uint64_t id = ESP.getEfuseMac();
  for (int i = 0; i < 8; i++)
  {
    _deviceId[7 - i] = (uint8_t)(id & 0xFF);
    id >>= 8;
  }
}

std::string JSBLEWrapper::GetDeviceIdHex() const
{
  return BytesToHex(_deviceId, 8);
}

std::string JSBLEWrapper::BuildAdvertisedName() const
{
  // Sista 2 bytes => 4 hex-tecken
  const uint8_t b6 = _deviceId[6];
  const uint8_t b7 = _deviceId[7];

  static const char* hex = "0123456789ABCDEF";
  char suffix[5];
  suffix[0] = hex[(b6 >> 4) & 0xF];
  suffix[1] = hex[b6 & 0xF];
  suffix[2] = hex[(b7 >> 4) & 0xF];
  suffix[3] = hex[b7 & 0xF];
  suffix[4] = '\0';

  return _deviceName + "-" + std::string(suffix);
}

void JSBLEWrapper::StartAdvertising()
{
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;

  adv->stop();

  NimBLEAdvertisementData adData;
  adData.setFlags(0x06); // LE General Discoverable + BR/EDR Not Supported
  adData.addServiceUUID(_serviceUUID);

  // Manufacturer Data:
  // CompanyId(2 LE) + 'J''S' + version(1) + deviceId(8)
  const uint16_t companyId = 0xFFFF; // intern/lab
  const uint8_t version = 0x01;

  uint8_t payload[2 + 2 + 1 + 8];
  payload[0] = (uint8_t)(companyId & 0xFF);
  payload[1] = (uint8_t)((companyId >> 8) & 0xFF);
  payload[2] = (uint8_t)'J';
  payload[3] = (uint8_t)'S';
  payload[4] = version;
  memcpy(&payload[5], _deviceId, 8);

  adData.setManufacturerData(std::string((char*)payload, sizeof(payload)));

  // Scan response: unikt namn
  NimBLEAdvertisementData srData;
  srData.setName(BuildAdvertisedName());

  adv->setAdvertisementData(adData);
  adv->setScanResponseData(srData);

  adv->start();
}

void JSBLEWrapper::Start()
{
  NimBLEDevice::init(_deviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  if (_server == nullptr)
  {
    _server = NimBLEDevice::createServer();

    _serverCallbacks = new JSBLEServerCallbacks(this);
    _server->setCallbacks(_serverCallbacks);

    _service = _server->createService(_serviceUUID);

    _txCharacteristic = _service->createCharacteristic(
      _characteristicTxUUID,
      NIMBLE_PROPERTY::NOTIFY
    );

    _rxCharacteristic = _service->createCharacteristic(
      _characteristicRxUUID,
      NIMBLE_PROPERTY::WRITE
    );

    _rxCallbacks = new JSBLECharacteristicCallbacks(this);
    _rxCharacteristic->setCallbacks(_rxCallbacks);

    _service->start();
  }

  StartAdvertising();

  Serial.print("BLE started. Name=");
  Serial.print(BuildAdvertisedName().c_str());
  Serial.print(" DeviceId=");
  Serial.println(GetDeviceIdHex().c_str());
}

void JSBLEWrapper::Stop()
{
  if (auto* adv = NimBLEDevice::getAdvertising())
    adv->stop();

  _deviceConnected = false;

  // Vill du riva ner allt helt:
  // NimBLEDevice::deinit(true);
}

void JSBLEWrapper::SendData(const std::string& command, const std::string& value)
{
  if (!_deviceConnected) return;
  if (_txCharacteristic == nullptr) return;
  if (command.size() != 2) return;

  const std::string payload = "AT" + command + value;
  _txCharacteristic->setValue(payload);
  _txCharacteristic->notify();

  delay(10);
}

void JSBLEWrapper::SetOnReceiveCallback(void (*onReceive)(std::string, std::string))
{
  _onReceiveData = onReceive;
}

void JSBLEWrapper::SetOnDisconnectedCallback(void (*onDisconnected)())
{
  _onDisconnectedCallback = onDisconnected;
}

void JSBLEWrapper::RegisterCommandHandler(const std::string& cmd, CommandHandler handler)
{
  if (cmd.size() != 2)
  {
    Serial.println("RegisterCommandHandler: cmd must be exactly 2 chars.");
    return;
  }
  _handlers[cmd] = handler;
}

void JSBLEWrapper::UnregisterCommandHandler(const std::string& cmd)
{
  _handlers.erase(cmd);
}

void JSBLEWrapper::ClearCommandHandlers()
{
  _handlers.clear();
}

bool JSBLEWrapper::TryDispatchCommand(const std::string& cmd, const std::string& value)
{
  auto it = _handlers.find(cmd);
  if (it == _handlers.end()) return false;
  if (it->second == nullptr) return false;

  it->second(this, cmd, value);
  return true;
}

// ===== Callbacks =====

JSBLECharacteristicCallbacks::JSBLECharacteristicCallbacks(JSBLEWrapper* owner)
  : _owner(owner)
{
}

void JSBLECharacteristicCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo&)
{
  if (_owner == nullptr) return;

  const std::string rxValue = pCharacteristic->getValue();

  if (rxValue.size() < 4) return;
  if (rxValue[0] != 'A' || rxValue[1] != 'T') return;

  const std::string cmd = rxValue.substr(2, 2);
  const std::string value = (rxValue.size() > 4) ? rxValue.substr(4) : "";

  if (_owner->TryDispatchCommand(cmd, value))
    return;

  if (_owner->_onReceiveData != nullptr)
    _owner->_onReceiveData(cmd, value);
}

JSBLEServerCallbacks::JSBLEServerCallbacks(JSBLEWrapper* owner)
  : _owner(owner)
{
}

void JSBLEServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&)
{
  if (_owner == nullptr) return;
  _owner->_deviceConnected = true;
}

void JSBLEServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int)
{
  if (_owner == nullptr) return;

  _owner->_deviceConnected = false;

  if (_owner->_onDisconnectedCallback != nullptr)
    _owner->_onDisconnectedCallback();

  // Snabb reconnect
  _owner->StartAdvertising();
}

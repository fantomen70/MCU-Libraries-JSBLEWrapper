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
                           const std::string& characteristicRx,
                           uint16_t deviceTypeId,
                           uint8_t maxConnections)
  : _deviceName(deviceName),
    _serviceUUID(serviceUUID),
    _characteristicTxUUID(characteristicTx),
    _characteristicRxUUID(characteristicRx),
    _deviceTypeId(deviceTypeId),
    _maxConnections(maxConnections < 1 ? 1 : (maxConnections > 3 ? 3 : maxConnections))
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
  // Sista 2 bytes av MAC => 4 hex-tecken suffix
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
  // CompanyId(2 LE) + 'J'(1) + 'S'(1) + version(1) + DeviceTypeId(2 LE)
  // Totalt 7 bytes.
  // DeviceTypeId identifierar enhetstypen, t.ex. 0x0001=BMS, 0x0002=Tank.
  // Appen filtrerar på 'J'+'S'+DeviceTypeId vid scanning.
  // Enheter av samma typ särskiljs via annonserat namn (BaseName-XXXX).
  const uint16_t companyId = 0xFFFF; // intern/lab
  const uint8_t  version   = 0x01;

  uint8_t payload[7];
  payload[0] = (uint8_t)(companyId & 0xFF);
  payload[1] = (uint8_t)((companyId >> 8) & 0xFF);
  payload[2] = (uint8_t)'J';
  payload[3] = (uint8_t)'S';
  payload[4] = version;
  payload[5] = (uint8_t)(_deviceTypeId & 0xFF);
  payload[6] = (uint8_t)((_deviceTypeId >> 8) & 0xFF);

  adData.setManufacturerData(std::string((char*)payload, sizeof(payload)));

  // Scan response: unikt namn (BaseName-XXXX)
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

  // NimBLE-Arduino tillåter upp till 3 samtidiga anslutningar som default
  // (styrs av CONFIG_BT_NIMBLE_MAX_CONNECTIONS i bibliotekets sdkconfig).
  // Större MTU ger snabbare överföring per notify.
  NimBLEDevice::setMTU(247);

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
  Serial.print(" TypeId=0x");
  Serial.print(_deviceTypeId, HEX);
  Serial.print(" MaxConnections=");
  Serial.print(_maxConnections);
  Serial.print(" DeviceId=");
  Serial.println(GetDeviceIdHex().c_str());
}

void JSBLEWrapper::Stop()
{
  if (auto* adv = NimBLEDevice::getAdvertising())
    adv->stop();

  _connectedCount = 0;
}

void JSBLEWrapper::SendData(const std::string& command, const std::string& value)
{
  if (_connectedCount == 0) return;
  if (_txCharacteristic == nullptr) return;
  if (command.size() != 2) return;

  const std::string payload = "AT" + command + value;
  _txCharacteristic->setValue(payload);
  _txCharacteristic->notify();  // notify skickar till alla prenumeranter

  delay(10);
}

void JSBLEWrapper::SetOnReceiveCallback(void (*onReceive)(std::string cmd, std::string value))
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

// ===== Password / Auth =====

static const char* PREFS_NAMESPACE = "jsble";
static const char* PREFS_KEY_PWD   = "pwd";

void JSBLEWrapper::SetPassword(const std::string& password)
{
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false); // false = read/write
  if (password.empty())
    prefs.remove(PREFS_KEY_PWD);
  else
    prefs.putString(PREFS_KEY_PWD, password.c_str());
  prefs.end();
}

std::string JSBLEWrapper::GetPassword() const
{
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // true = read-only
  String s = prefs.getString(PREFS_KEY_PWD, "");
  prefs.end();
  return std::string(s.c_str());
}

bool JSBLEWrapper::IsAuthRequired() const
{
  return !GetPassword().empty();
}

bool JSBLEWrapper::TryDispatchCommand(uint16_t connHandle, const std::string& cmd, const std::string& value)
{
  // AS = Auth Status. Tillåts alltid utan auth – appen använder den för att
  // veta om enheten kräver lösenord.
  if (cmd == "AS")
  {
    SendData("AS", IsAuthRequired() ? "1" : "0");
    return true;
  }

  // CP = Change Password. Specialregel:
  //   - Tomt NVS (factory mode): tillåts utan auth (annars omöjligt att sätta
  //     lösenord första gången från appen)
  //   - Lösen satt: kräver auth som vilket annat kommando som helst
  // Tomt value betyder "ta bort lösenord".
  if (cmd == "CP")
  {
    if (IsAuthRequired())
    {
      auto it = _sessions.find(connHandle);
      const bool authorized = (it != _sessions.end() && it->second.authorized);
      if (!authorized)
      {
        Serial.println("CP avvisat: kräver auth när lösenord redan är satt.");
        SendData("ER", "AU");
        return true;
      }
    }

    SetPassword(value);

    if (value.empty())
      Serial.println("Lösenord rensat via CP.");
    else
      Serial.println("Lösenord ändrat via CP.");

    // Den ändrade sessionen får behålla sin auth-status även efter ändring –
    // den klienten har ju precis bevisat att de känner till nya tillståndet.
    // Andra sessioner är fortfarande auktoriserade tills de kopplar ner –
    // det är acceptabelt eftersom de redan visste det gamla lösenordet.
    auto it = _sessions.find(connHandle);
    if (it != _sessions.end())
      it->second.authorized = true;

    SendData("AK", "CP");
    return true;
  }

  // Auth-flöde: hantera "PW" specialfall innan vi tittar på handlers.
  if (cmd == "PW")
  {
    auto& session = _sessions[connHandle];
    const std::string stored = GetPassword();

    if (stored.empty())
    {
      // Ingen auth krävs på enheten – behandla PW som no-op-ack.
      session.authorized = true;
      SendData("AK", "PW");
      return true;
    }

    if (value == stored)
    {
      session.authorized = true;
      session.failedAttempts = 0;
      Serial.print("Auth OK på conn ");
      Serial.println(connHandle);
      SendData("AK", "PW");
    }
    else
    {
      session.failedAttempts++;
      Serial.print("Auth fel på conn ");
      Serial.print(connHandle);
      Serial.print(" (försök ");
      Serial.print(session.failedAttempts);
      Serial.print("/");
      Serial.print(MAX_AUTH_FAILURES);
      Serial.println(")");

      SendData("ER", "PW");

      if (session.failedAttempts >= MAX_AUTH_FAILURES)
      {
        Serial.println("För många auth-fel, kopplar ner klient.");
        DisconnectClient(connHandle);
      }
    }
    return true;
  }

  // Alla andra kommandon kräver auth om lösenord är satt.
  if (IsAuthRequired())
  {
    auto it = _sessions.find(connHandle);
    const bool authorized = (it != _sessions.end() && it->second.authorized);

    if (!authorized)
    {
      Serial.print("Avvisar ");
      Serial.print(cmd.c_str());
      Serial.print(" från oauktoriserad conn ");
      Serial.println(connHandle);
      SendData("ER", "AU"); // Auth-fel
      return true; // konsumerat – vi har "hanterat" det genom att avvisa
    }
  }

  // Normal dispatch
  auto it = _handlers.find(cmd);
  if (it == _handlers.end()) return false;
  if (it->second == nullptr) return false;

  it->second(this, cmd, value);
  return true;
}

void JSBLEWrapper::DisconnectClient(uint16_t connHandle)
{
  if (_server == nullptr) return;
  _server->disconnect(connHandle);
}

// ===== Callbacks =====

JSBLECharacteristicCallbacks::JSBLECharacteristicCallbacks(JSBLEWrapper* owner)
  : _owner(owner)
{
}

void JSBLECharacteristicCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo)
{
  if (_owner == nullptr) return;

  const std::string rxValue = pCharacteristic->getValue();

  if (rxValue.size() < 4) return;
  if (rxValue[0] != 'A' || rxValue[1] != 'T') return;

  const std::string cmd   = rxValue.substr(2, 2);
  const std::string value = (rxValue.size() > 4) ? rxValue.substr(4) : "";

  const uint16_t connHandle = connInfo.getConnHandle();

  if (_owner->TryDispatchCommand(connHandle, cmd, value))
    return;

  // Om kommandot inte hade en handler men auth krävs och vi är oauktoriserade
  // har TryDispatchCommand redan returnerat true (avvisat). Hit kommer bara
  // okända kommandon från auktoriserade sessioner.
  if (_owner->_onReceiveData != nullptr)
    _owner->_onReceiveData(cmd, value);
}

JSBLEServerCallbacks::JSBLEServerCallbacks(JSBLEWrapper* owner)
  : _owner(owner)
{
}

void JSBLEServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo& connInfo)
{
  if (_owner == nullptr) return;
  _owner->_connectedCount++;

  // Skapa tom session-state. Klienten är ej auktoriserad än.
  _owner->_sessions[connInfo.getConnHandle()] = JSBLEWrapper::SessionState{};

  // Starta om annonseringen bara om det finns plats för fler klienter.
  // Vid full kapacitet låter vi annonseringen vara stoppad – då vet andra
  // appar att enheten inte är tillgänglig och vi sparar lite ström.
  if (_owner->_connectedCount < _owner->_maxConnections)
    _owner->StartAdvertising();
}

void JSBLEServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int)
{
  if (_owner == nullptr) return;

  if (_owner->_connectedCount > 0)
    _owner->_connectedCount--;

  // Rensa sessions-state – auth gäller bara per session.
  _owner->_sessions.erase(connInfo.getConnHandle());

  // Anropa disconnect-callback bara när alla klienter har kopplat ner.
  if (_owner->_connectedCount == 0 && _owner->_onDisconnectedCallback != nullptr)
    _owner->_onDisconnectedCallback();

  // Det finns nu plats igen – återuppta annonseringen.
  _owner->StartAdvertising();
}

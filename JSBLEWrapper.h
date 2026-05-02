#ifndef JSBLEWRAPPER_H
#define JSBLEWRAPPER_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <string>
#include <unordered_map>

/**
 * @brief Enkel wrapper runt ESP32 BLE Server (NimBLE-Arduino 2.3.7)
 *
 * - Startar BLE-tjänst
 * - Tar emot AT-kommandon: "AT" + cmd(2 chars) + value
 * - Skickar notifieringar (TX notify)
 * - Tar emot data (RX write)
 * - Stöd för kommandodispatch (registrera handler per cmd)
 * - Advertisar Manufacturer Data med 'JS' + version + DeviceTypeId
 *
 * Manufacturer Data-layout (7 bytes):
 *   [0-1] CompanyId   (0xFFFF, little-endian)
 *   [2]   'J'
 *   [3]   'S'
 *   [4]   version     (0x01)
 *   [5-6] DeviceTypeId (uint16_t, little-endian)
 *
 * Appen filtrerar på 'J'+'S'+DeviceTypeId vid scanning.
 * Flera enheter av samma typ särskiljs via annonserat namn (BaseName-XXXX).
 *
 * Ingen bakgrundstask behövs.
 */
class JSBLEWrapper
{
public:
  using CommandHandler = void (*)(JSBLEWrapper* self, const std::string& cmd, const std::string& value);

  /**
   * @param deviceName   Basnamn utan suffix, t.ex. "ChargeCtrl"
   * @param serviceUUID  GATT Service UUID
   * @param characteristicTx  TX karakteristika UUID (NOTIFY, ESP32→App)
   * @param characteristicRx  RX karakteristika UUID (WRITE, App→ESP32)
   * @param deviceTypeId Identifierar enhetstypen, t.ex. 0x0001=BMS, 0x0002=Tank
   * @param maxConnections Max antal samtidiga klienter (1–3, default 1).
   *                       Vid full kapacitet stoppas annonsering – enheten
   *                       blir osynlig i scan tills någon klient kopplar ner.
   *                       Begränsat till 3 av NimBLE-Arduinos default-konfig.
   */
  JSBLEWrapper(const std::string& deviceName,
               const std::string& serviceUUID,
               const std::string& characteristicTx,
               const std::string& characteristicRx,
               uint16_t deviceTypeId,
               uint8_t maxConnections = 1);

  void Start();
  void Stop();

  /// Payload: "AT" + command(2) + value
  void SendData(const std::string& command, const std::string& value);

  void SetOnReceiveCallback(void (*onReceive)(std::string cmd, std::string value));
  void SetOnDisconnectedCallback(void (*onDisconnected)());

  void RegisterCommandHandler(const std::string& cmd, CommandHandler handler);
  void UnregisterCommandHandler(const std::string& cmd);
  void ClearCommandHandlers();

  /// MAC-baserat enhets-ID (hex, 16 tecken) – används för namnets XXXX-suffix
  std::string GetDeviceIdHex() const;

  /// Antal aktiva BLE-anslutningar just nu (0 om ingen klient är ansluten).
  int GetConnectedCount() const { return _connectedCount; }

  /// Max antal samtidiga klienter, satt vid konstruktion.
  uint8_t GetMaxConnections() const { return _maxConnections; }

  /**
   * Sätter lösenord för auth. Tom sträng = ingen auth krävs.
   * Lagras persistent i NVS – överlever omstart och omflashning.
   * Aktiva sessioner påverkas inte (kopplas inte ner) – nya anslutningar
   * möter det nya lösenordet.
   */
  void SetPassword(const std::string& password);

  /**
   * Returnerar nuvarande lösenord från NVS, eller tom sträng om inget satt.
   */
  std::string GetPassword() const;

  /**
   * True om auth krävs (dvs lösenord är satt).
   */
  bool IsAuthRequired() const;

  /// Max antal felaktiga PW-försök per session innan klienten kopplas ner.
  static constexpr int MAX_AUTH_FAILURES = 3;

private:
  bool TryDispatchCommand(uint16_t connHandle, const std::string& cmd, const std::string& value);

  void BuildDeviceId();
  void StartAdvertising();

  /// "BaseName-XXXX" där XXXX är sista 4 hex av MAC
  std::string BuildAdvertisedName() const;

  /// Antal aktiva anslutningar. > 0 betyder att minst en klient lyssnar.
  int _connectedCount = 0;

  std::string _deviceName;
  std::string _serviceUUID;
  std::string _characteristicTxUUID;
  std::string _characteristicRxUUID;
  uint16_t    _deviceTypeId;
  uint8_t     _maxConnections;

  NimBLEServer*         _server = nullptr;
  NimBLEService*        _service = nullptr;
  NimBLECharacteristic* _txCharacteristic = nullptr;
  NimBLECharacteristic* _rxCharacteristic = nullptr;

  class JSBLECharacteristicCallbacks* _rxCallbacks = nullptr;
  class JSBLEServerCallbacks*         _serverCallbacks = nullptr;

  void (*_onReceiveData)(std::string, std::string) = nullptr;
  void (*_onDisconnectedCallback)() = nullptr;

  std::unordered_map<std::string, CommandHandler> _handlers;

  /// Per-session auth state. Nyckel: NimBLE connection handle.
  struct SessionState {
    bool authorized = false;
    int  failedAttempts = 0;
  };
  std::unordered_map<uint16_t, SessionState> _sessions;

  /// Tvingar nedkoppling av en klient (efter för många auth-fel).
  void DisconnectClient(uint16_t connHandle);

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

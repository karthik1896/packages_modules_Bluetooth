/******************************************************************************
 *
 *  Copyright 2019 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <variant>

#include "common/bind.h"
#include "crypto_toolbox/crypto_toolbox.h"
#include "hci/hci_packets.h"
#include "hci/le_security_interface.h"
#include "packet/packet_view.h"
#include "security/ecdh_keys.h"
#include "security/initial_informations.h"
#include "security/pairing_failure.h"
#include "security/smp_packets.h"
#include "security/ui.h"

// Code generated by PDL does not allow us ot do || and && operations on bits
// efficiently. Use those masks on fields requiring them until this is solved
constexpr uint8_t AuthReqMaskBondingFlag = 0x01;
constexpr uint8_t AuthReqMaskMitm = 0x04;
constexpr uint8_t AuthReqMaskSc = 0x08;
constexpr uint8_t AuthReqMaskKeypress = 0x10;
constexpr uint8_t AuthReqMaskCt2 = 0x20;

constexpr uint8_t KeyMaskEnc = 0x01;
constexpr uint8_t KeyMaskId = 0x02;
constexpr uint8_t KeyMaskSign = 0x04;
constexpr uint8_t KeyMaskLink = 0x08;

using bluetooth::hci::EncryptionChangeView;
using bluetooth::hci::EncryptionKeyRefreshCompleteView;

namespace bluetooth {
namespace security {

/* This class represents an event send from other subsystems into SMP Pairing Handler,
 * i.e. user request from the UI, L2CAP or HCI interaction */
class PairingEvent {
 public:
  enum TYPE { EXIT, L2CAP, HCI_EVENT, UI };
  TYPE type;

  std::optional<CommandView> l2cap_packet;

  std::optional<hci::EventView> hci_event;

  enum UI_ACTION_TYPE { PAIRING_ACCEPTED, CONFIRM_YESNO, PASSKEY };
  UI_ACTION_TYPE ui_action;
  uint32_t ui_value;

  PairingEvent(TYPE type) : type(type) {}
  PairingEvent(CommandView l2cap_packet) : type(L2CAP), l2cap_packet(l2cap_packet) {}
  PairingEvent(UI_ACTION_TYPE ui_action, uint32_t ui_value) : type(UI), ui_action(ui_action), ui_value(ui_value) {}
  PairingEvent(hci::EventView hci_event) : type(HCI_EVENT), hci_event(hci_event) {}
};

constexpr int SMP_TIMEOUT = 30;

using CommandViewOrFailure = std::variant<CommandView, PairingFailure>;
using Phase1Result = std::pair<PairingRequestView /* pairning_request*/, PairingResponseView /* pairing_response */>;
using Phase1ResultOrFailure = std::variant<PairingFailure, Phase1Result>;
using KeyExchangeResult =
    std::tuple<EcdhPublicKey /* PKa */, EcdhPublicKey /* PKb */, std::array<uint8_t, 32> /*dhkey*/>;
using Stage1Result = std::tuple<hci::Octet16, hci::Octet16, hci::Octet16, hci::Octet16>;
using Stage1ResultOrFailure = std::variant<PairingFailure, Stage1Result>;
using Stage2ResultOrFailure = std::variant<PairingFailure, hci::Octet16 /* LTK */>;
using DistributedKeysOrFailure = std::variant<PairingFailure, DistributedKeys, std::monostate>;

using LegacyStage1Result = hci::Octet16 /*TK*/;
using LegacyStage1ResultOrFailure = std::variant<PairingFailure, LegacyStage1Result>;
using StkOrFailure = std::variant<PairingFailure, hci::Octet16 /* STK */>;

/* PairingHandlerLe takes care of the Pairing process. Pairing is strictly defined
 * exchange of messages and UI interactions, divided into PHASES.
 *
 * Each PairingHandlerLe have a thread executing |PairingMain| method. Thread is
 * blocked when waiting for UI/L2CAP/HCI interactions, and moves through all the
 * phases.
 */
class PairingHandlerLe {
 public:
  // This is the phase of pairing as defined in BT Spec (with exception of
  // accept prompt)
  // * ACCEPT_PROMPT - we're waiting for the user to accept remotely initiated pairing
  // * PHASE1 - feature exchange
  // * PHASE2 - authentication
  // * PHASE3 - key exchange
  enum PAIRING_PHASE { ACCEPT_PROMPT, PHASE1, PHASE2, PHASE3 };
  PAIRING_PHASE phase;

  // All the knowledge to initiate the pairing process must be passed into this function
  PairingHandlerLe(PAIRING_PHASE phase, InitialInformations informations)
      : phase(phase), queue_guard(), thread_(&PairingHandlerLe::PairingMain, this, informations) {}

  ~PairingHandlerLe() {
    SendExitSignal();
    // we need ot check if thread is joinable, because tests call join form
    // within WaitUntilPairingFinished
    if (thread_.joinable()) thread_.join();
  }

  void PairingMain(InitialInformations i);

  Phase1ResultOrFailure ExchangePairingFeature(const InitialInformations& i);

  void SendL2capPacket(const InitialInformations& i, std::unique_ptr<bluetooth::security::CommandBuilder> command) {
    i.proper_l2cap_interface->Enqueue(std::move(command), i.l2cap_handler);
  }

  void SendHciLeStartEncryption(
      const InitialInformations& i,
      uint16_t conn_handle,
      const std::array<uint8_t, 8>& rand,
      const uint16_t& ediv,
      const hci::Octet16& ltk) {
    i.le_security_interface->EnqueueCommand(hci::LeStartEncryptionBuilder::Create(conn_handle, rand, ediv, ltk),
                                            i.l2cap_handler->BindOnce([](hci::CommandStatusView) {
                                              // TODO: handle command status. It's important - can show we are not
                                              // connected any more.

                                              // TODO: if anything useful must be done there, use some sort of proper
                                              // handler, wait/notify, and execute on the handler thread
                                            }));
  }

  void SendHciLeLongTermKeyReply(
      const InitialInformations& i, uint16_t conn_handle, const hci::Octet16& ltk) {
    i.le_security_interface->EnqueueCommand(
        hci::LeLongTermKeyRequestReplyBuilder::Create(conn_handle, ltk),
        i.l2cap_handler->BindOnce([](hci::CommandCompleteView) {}));
  }

  std::variant<PairingFailure, EncryptionChangeView, EncryptionKeyRefreshCompleteView> WaitEncryptionChanged() {
    PairingEvent e = WaitForEvent();
    if (e.type != PairingEvent::HCI_EVENT) return PairingFailure("Was expecting HCI event but received something else");

    if (!e.hci_event->IsValid()) return PairingFailure("Received invalid HCI event");

    if (e.hci_event->GetEventCode() == hci::EventCode::ENCRYPTION_CHANGE) {
      EncryptionChangeView enc_chg_packet = EncryptionChangeView::Create(*e.hci_event);
      if (!enc_chg_packet.IsValid()) {
        return PairingFailure("Invalid Encryption Change packet received");
      }
      return enc_chg_packet;
    }

    if (e.hci_event->GetEventCode() == hci::EventCode::ENCRYPTION_KEY_REFRESH_COMPLETE) {
      hci::EncryptionKeyRefreshCompleteView enc_packet = EncryptionKeyRefreshCompleteView::Create(*e.hci_event);
      if (!enc_packet.IsValid()) {
        return PairingFailure("Invalid Key Refresh packet received");
      }
      return enc_packet;
    }

    return PairingFailure("Was expecting Encryption Change or Key Refresh Complete but received something else");
  }

  std::variant<PairingFailure, hci::LeLongTermKeyRequestView> WaitLeLongTermKeyRequest() {
    PairingEvent e = WaitForEvent();
    if (e.type != PairingEvent::HCI_EVENT) return PairingFailure("Was expecting HCI event but received something else");

    if (!e.hci_event->IsValid()) return PairingFailure("Received invalid HCI event");

    if (e.hci_event->GetEventCode() != hci::EventCode::LE_META_EVENT) return PairingFailure("Was expecting LE event");

    hci::LeMetaEventView le_event = hci::LeMetaEventView::Create(*e.hci_event);
    if (!le_event.IsValid()) {
      return PairingFailure("Invalid LE Event received");
    }

    if (le_event.GetSubeventCode() != hci::SubeventCode::LONG_TERM_KEY_REQUEST) {
      return PairingFailure("Was expecting Long Term Key Request");
    }

    hci::LeLongTermKeyRequestView ltk_req_packet = hci::LeLongTermKeyRequestView::Create(le_event);
    if (!ltk_req_packet.IsValid()) {
      return PairingFailure("Invalid LE Long Term Key Request received");
    }

    return ltk_req_packet;
  }

  inline bool IAmCentral(const InitialInformations& i) {
    return i.my_role == hci::Role::CENTRAL;
  }

  /* This function generates data that should be passed to remote device, except
     the private key. */
  static MyOobData GenerateOobData();

  std::variant<PairingFailure, KeyExchangeResult> ExchangePublicKeys(const InitialInformations& i,
                                                                     OobDataFlag remote_have_oob_data);

  Stage1ResultOrFailure DoSecureConnectionsStage1(const InitialInformations& i, const EcdhPublicKey& PKa,
                                                  const EcdhPublicKey& PKb, const PairingRequestView& pairing_request,
                                                  const PairingResponseView& pairing_response);

  Stage1ResultOrFailure SecureConnectionsNumericComparison(const InitialInformations& i, const EcdhPublicKey& PKa,
                                                           const EcdhPublicKey& PKb);

  Stage1ResultOrFailure SecureConnectionsJustWorks(const InitialInformations& i, const EcdhPublicKey& PKa,
                                                   const EcdhPublicKey& PKb);

  Stage1ResultOrFailure SecureConnectionsPasskeyEntry(const InitialInformations& i, const EcdhPublicKey& PKa,
                                                      const EcdhPublicKey& PKb, IoCapability my_iocaps,
                                                      IoCapability remote_iocaps);

  Stage1ResultOrFailure SecureConnectionsOutOfBand(const InitialInformations& i, const EcdhPublicKey& Pka,
                                                   const EcdhPublicKey& Pkb, OobDataFlag my_oob_flag,
                                                   OobDataFlag remote_oob_flag);

  Stage2ResultOrFailure DoSecureConnectionsStage2(const InitialInformations& i, const EcdhPublicKey& PKa,
                                                  const EcdhPublicKey& PKb, const PairingRequestView& pairing_request,
                                                  const PairingResponseView& pairing_response,
                                                  const Stage1Result stage1result,
                                                  const std::array<uint8_t, 32>& dhkey);

  DistributedKeysOrFailure DistributeKeys(const InitialInformations& i, const PairingResponseView& pairing_response,
                                          bool isSecureConnections);

  DistributedKeysOrFailure ReceiveKeys(const uint8_t& keys_i_receive);

  LegacyStage1ResultOrFailure DoLegacyStage1(const InitialInformations& i, const PairingRequestView& pairing_request,
                                             const PairingResponseView& pairing_response);
  LegacyStage1ResultOrFailure LegacyOutOfBand(const InitialInformations& i);
  LegacyStage1ResultOrFailure LegacyJustWorks();
  LegacyStage1ResultOrFailure LegacyPasskeyEntry(const InitialInformations& i, const IoCapability& my_iocaps,
                                                 const IoCapability& remote_iocaps);
  StkOrFailure DoLegacyStage2(
      const InitialInformations& i,
      const PairingRequestView& pairing_request,
      const PairingResponseView& pairing_response,
      const hci::Octet16& tk);

  void SendKeys(
      const InitialInformations& i,
      const uint8_t& keys_i_send,
      hci::Octet16 ltk,
      uint16_t ediv,
      std::array<uint8_t, 8> rand,
      hci::Octet16 irk,
      Address identity_address,
      AddrType identity_addres_type,
      hci::Octet16 signature_key);

  /* This can be called from any thread to immediately finish the pairing in progress. */
  void SendExitSignal() {
    {
      std::unique_lock<std::mutex> lock(queue_guard);
      queue.push(PairingEvent(PairingEvent::EXIT));
    }
    pairing_thread_blocker_.notify_one();
  }

  /* SMP Command received from remote device */
  void OnCommandView(CommandView packet) {
    {
      std::unique_lock<std::mutex> lock(queue_guard);
      queue.push(PairingEvent(std::move(packet)));
    }
    pairing_thread_blocker_.notify_one();
  }

  /* SMP Command received from remote device */
  void OnHciEvent(hci::EventView hci_event) {
    {
      std::unique_lock<std::mutex> lock(queue_guard);
      queue.push(PairingEvent(std::move(hci_event)));
    }
    pairing_thread_blocker_.notify_one();
  }

  /* Interaction from user */
  void OnUiAction(PairingEvent::UI_ACTION_TYPE ui_action, uint32_t ui_value) {
    {
      std::unique_lock<std::mutex> lock(queue_guard);
      queue.push(PairingEvent(ui_action, ui_value));
    }
    pairing_thread_blocker_.notify_one();
  }

  /* HCI LE event received from remote device */
  void OnHciLeEvent(hci::LeMetaEventView hci_event) {
    {
      std::unique_lock<std::mutex> lock(queue_guard);
      queue.push(PairingEvent(std::move(hci_event)));
    }
    pairing_thread_blocker_.notify_one();
  }

  /* Blocks the pairing process until some external interaction, or timeout happens */
  PairingEvent WaitForEvent() {
    std::unique_lock<std::mutex> lock(queue_guard);
    do {
      if (!queue.empty()) {
        PairingEvent e = queue.front();
        queue.pop();
        return e;
      }
      // This releases the lock while blocking.
      if (pairing_thread_blocker_.wait_for(lock, std::chrono::seconds(SMP_TIMEOUT)) == std::cv_status::timeout) {
        return PairingEvent(PairingEvent::EXIT);
      }

    } while (true);
  }

  std::optional<PairingEvent> WaitUiPairingAccept() {
    PairingEvent e = WaitForEvent();
    if (e.type == PairingEvent::UI &&
        e.ui_action == PairingEvent::PAIRING_ACCEPTED) {
      return e;
    } else {
      return std::nullopt;
    }
  }

  std::optional<PairingEvent> WaitUiConfirmYesNo() {
    PairingEvent e = WaitForEvent();
    if (e.type == PairingEvent::UI &&
        e.ui_action == PairingEvent::CONFIRM_YESNO) {
      return e;
    } else {
      return std::nullopt;
    }
  }

  std::optional<PairingEvent> WaitUiPasskey() {
    PairingEvent e = WaitForEvent();

    // It's possible to receive PAIRING_CONFIRM from remote device while waiting for the passkey.
    // Store it until it's needed.
    if (e.type == PairingEvent::L2CAP) {
      auto l2cap_packet = e.l2cap_packet.value();
      if (!l2cap_packet.IsValid()) {
        LOG_WARN("Malformed L2CAP packet received!");
        return std::nullopt;
      }

      const auto& received_code = l2cap_packet.GetCode();
      if (received_code != Code::PAIRING_CONFIRM) {
        LOG_WARN("Was waiting for passkey, received bad packet instead!");
        return std::nullopt;
      }

      auto pkt = PairingConfirmView::Create(l2cap_packet);
      if (!pkt.IsValid()) {
        LOG_WARN("Malformed PAIRING_CONFIRM packet");
        return std::nullopt;
      }

      cached_pariring_confirm_view = std::make_unique<PairingConfirmView>(pkt);
      e = WaitForEvent();
    }

    if (e.type == PairingEvent::UI &&
        e.ui_action == PairingEvent::PASSKEY) {
      return e;
    } else {
      return std::nullopt;
    }
  }

  template <Code C>
  struct CodeToPacketView;
  template <>
  struct CodeToPacketView<Code::PAIRING_REQUEST> {
    typedef PairingRequestView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_RESPONSE> {
    typedef PairingResponseView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_CONFIRM> {
    typedef PairingConfirmView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_RANDOM> {
    typedef PairingRandomView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_FAILED> {
    typedef PairingFailedView type;
  };
  template <>
  struct CodeToPacketView<Code::ENCRYPTION_INFORMATION> {
    typedef EncryptionInformationView type;
  };
  template <>
  struct CodeToPacketView<Code::CENTRAL_IDENTIFICATION> {
    typedef CentralIdentificationView type;
  };
  template <>
  struct CodeToPacketView<Code::IDENTITY_INFORMATION> {
    typedef IdentityInformationView type;
  };
  template <>
  struct CodeToPacketView<Code::IDENTITY_ADDRESS_INFORMATION> {
    typedef IdentityAddressInformationView type;
  };
  template <>
  struct CodeToPacketView<Code::SIGNING_INFORMATION> {
    typedef SigningInformationView type;
  };
  template <>
  struct CodeToPacketView<Code::SECURITY_REQUEST> {
    typedef SecurityRequestView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_PUBLIC_KEY> {
    typedef PairingPublicKeyView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_DH_KEY_CHECK> {
    typedef PairingDhKeyCheckView type;
  };
  template <>
  struct CodeToPacketView<Code::PAIRING_KEYPRESS_NOTIFICATION> {
    typedef PairingKeypressNotificationView type;
  };

  template <Code CODE>
  std::variant<typename CodeToPacketView<CODE>::type, PairingFailure> WaitPacket() {
    PairingEvent e = WaitForEvent();
    switch (e.type) {
      case PairingEvent::EXIT:
        return PairingFailure(
            /*FROM_HERE,*/ "Was expecting L2CAP Packet " + CodeText(CODE) + ", but received EXIT instead");

      case PairingEvent::HCI_EVENT:
        return PairingFailure(
            /*FROM_HERE,*/ "Was expecting L2CAP Packet " + CodeText(CODE) + ", but received HCI_EVENT instead");

      case PairingEvent::UI:
        return PairingFailure(
            /*FROM_HERE,*/ "Was expecting L2CAP Packet " + CodeText(CODE) + ", but received UI instead");

      case PairingEvent::L2CAP: {
        auto l2cap_packet = e.l2cap_packet.value();
        if (!l2cap_packet.IsValid()) {
          return PairingFailure("Malformed L2CAP packet received!");
        }

        const auto& received_code = l2cap_packet.GetCode();
        if (received_code != CODE) {
          if (received_code == Code::PAIRING_FAILED) {
            auto pkt = PairingFailedView::Create(l2cap_packet);
            if (!pkt.IsValid()) return PairingFailure("Malformed " + CodeText(CODE) + " packet");
            return PairingFailure(/*FROM_HERE,*/
                                  "Was expecting " + CodeText(CODE) + ", but received PAIRING_FAILED instead",
                                  pkt.GetReason());
          }

          return PairingFailure(/*FROM_HERE,*/
                                "Was expecting " + CodeText(CODE) + ", but received " + CodeText(received_code) +
                                    " instead",
                                received_code);
        }

        auto pkt = CodeToPacketView<CODE>::type::Create(l2cap_packet);
        if (!pkt.IsValid()) return PairingFailure("Malformed " + CodeText(CODE) + " packet");
        return pkt;
      }
    }
  }

  auto WaitPairingRequest() {
    return WaitPacket<Code::PAIRING_REQUEST>();
  }

  auto WaitPairingResponse() {
    return WaitPacket<Code::PAIRING_RESPONSE>();
  }

  std::variant<bluetooth::security::PairingConfirmView, bluetooth::security::PairingFailure> WaitPairingConfirm() {
    if (cached_pariring_confirm_view) {
      PairingConfirmView pkt = *cached_pariring_confirm_view;
      cached_pariring_confirm_view.release();
      return pkt;
    }
    return WaitPacket<Code::PAIRING_CONFIRM>();
  }

  auto WaitPairingRandom() {
    return WaitPacket<Code::PAIRING_RANDOM>();
  }

  auto WaitPairingPublicKey() {
    return WaitPacket<Code::PAIRING_PUBLIC_KEY>();
  }

  auto WaitPairingDHKeyCheck() {
    return WaitPacket<Code::PAIRING_DH_KEY_CHECK>();
  }

  auto WaitEncryptionInformationRequest() {
    return WaitPacket<Code::ENCRYPTION_INFORMATION>();
  }

  auto WaitEncryptionInformation() {
    return WaitPacket<Code::ENCRYPTION_INFORMATION>();
  }

  auto WaitCentralIdentification() {
    return WaitPacket<Code::CENTRAL_IDENTIFICATION>();
  }

  auto WaitIdentityInformation() {
    return WaitPacket<Code::IDENTITY_INFORMATION>();
  }

  auto WaitIdentityAddressInformation() {
    return WaitPacket<Code::IDENTITY_ADDRESS_INFORMATION>();
  }

  auto WaitSigningInformation() {
    return WaitPacket<Code::SIGNING_INFORMATION>();
  }

  /* This is just for test, never use in production code! */
  void WaitUntilPairingFinished() {
    thread_.join();
  }

 private:
  std::condition_variable pairing_thread_blocker_;

  std::mutex queue_guard;
  std::queue<PairingEvent> queue;

  std::thread thread_;

  // holds pairing_confirm, if received out of order
  std::unique_ptr<PairingConfirmView> cached_pariring_confirm_view;
};
}  // namespace security
}  // namespace bluetooth

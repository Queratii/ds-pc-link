/*
    ESP32MP — interface multijoueur de melonDS qui passe par un ESP32.

    Fait partie de ds-pc-link (voir melonds-esp32/README.md). Ajouté à
    melonDS, il est distribué sous la même licence : GNU GPL v3 ou ultérieure.

    melonDS émule une DS HÔTE ; un ESP32 flashé avec esp32-bridge lui prête sa
    radio, en mode MODE_MP_HOST. De vraies DS rejoignent la partie comme
    clientes, avec le multijoueur natif du jeu (cartouche ou téléchargement).

    Activation : variable d'environnement MELONDS_ESP32_PORT avant de lancer
    melonDS, par exemple MELONDS_ESP32_PORT=COM5. L'interface « locale » de
    melonDS est alors remplacée par celle-ci. Facultatif :
    MELONDS_ESP32_WINDOW_US, durée de la fenêtre de réponses (3000 par défaut).

    Limites de cette version :
      - une seule instance de melonDS, qui doit être l'HÔTE de la partie ;
      - melonDS client d'une vraie DS hôte n'est pas géré (RecvHostPacket ne
        reçoit rien) : l'ESP32 devrait alors répondre seul aux CMD en
        quelques centaines de µs.
*/

#ifndef ESP32MP_H
#define ESP32MP_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "types.h"
#include "MPInterface.h"

namespace melonDS
{

class ESP32MP : public MPInterface
{
public:
    // Vrai si MELONDS_ESP32_PORT est défini : MPInterface::Set() choisit
    // alors cette interface à la place de l'interface locale.
    static bool Configured() noexcept;

    ESP32MP() noexcept;
    ~ESP32MP() noexcept override;
    ESP32MP(const ESP32MP&) = delete;
    ESP32MP& operator=(const ESP32MP&) = delete;

    void Process() override {}

    void Begin(int inst) override;
    void End(int inst) override;

    int SendPacket(int inst, u8* data, int len, u64 timestamp) override;
    int RecvPacket(int inst, u8* data, u64* timestamp) override;
    int SendCmd(int inst, u8* data, int len, u64 timestamp) override;
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) override;
    int SendAck(int inst, u8* data, int len, u64 timestamp) override;
    int RecvHostPacket(int inst, u8* data, u64* timestamp) override;
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) override;

private:
    struct Reply
    {
        u16 AID;
        std::vector<u8> Buf;     // tampon matériel DS : en-tête 12 o + trame + FCS
    };

    // Port série
    bool OpenPort(const std::string& name) noexcept;
    void ClosePort() noexcept;
    bool WritePort(const u8* data, size_t len) noexcept;
    int ReadPort(u8* data, size_t len) noexcept;       // attend au plus ~20 ms

    // Protocole série de ds-pc-link (COBS + CRC16, voir common/linkproto.h)
    void SendMsg(u8 type, const u8* payload, size_t len) noexcept;
    void SendMPTx(u8 flags, u8 id, const u8* data, int len) noexcept;
    void ReaderLoop() noexcept;
    void HandleMsg(const u8* msg, size_t len) noexcept;

    std::vector<u8> MakeDSBuffer(u8 rate, const u8* frame, size_t len) const;
    void NoteHostFrame(const u8* data, int len, u64 timestamp) noexcept;

#ifdef _WIN32
    void* Port = nullptr;                    // HANDLE
#else
    int Port = -1;
#endif
    std::thread Reader;
    std::atomic<bool> Stop {false};
    std::mutex WriteLock;

    std::mutex Lock;                         // protège tout ce qui suit
    std::condition_variable ReplyCV;
    std::deque<std::vector<u8>> RXQueue;     // trames pour RecvPacket()
    std::vector<Reply> Replies;              // réponses à la CMD courante
    u16 ReplyMask = 0;                       // bit n = cliente n a répondu
    u8 CmdID = 0;                            // n° de la CMD courante
    bool WindowDone = true;
    std::map<u64, u16> MACToAID;             // appris des réponses d'association

    std::atomic<u8> Channel {0};             // canal de l'hôte émulé
    std::atomic<u64> LastTimestamp {1};      // dernier horodatage de l'hôte émulé
    int HostInst = -1;
    u32 LastLate = 0;
};

}

#endif // ESP32MP_H

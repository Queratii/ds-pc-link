/*
    ESP32MP — interface multijoueur de melonDS qui passe par un ESP32.
    Voir ESP32MP.h. Licence : GNU GPL v3 ou ultérieure, comme melonDS.

    Déroulement, melonDS étant la DS hôte :

      melonDS                         ESP32 (MODE_MP_HOST)          vraies DS
      SendPacket(beacon, assoc…) ──>  émet, prend la MAC de l'hôte  ──> clientes
      RecvPacket()               <──  auth / assoc / probe reçues   <── clientes
      SendCmd(CMD)               ──>  émet, ouvre la fenêtre        ──> clientes
      RecvReplies() attend       <──  MP_REPLY…, puis MP_WINDOW     <── réponses
      SendAck(ACK)               ──>  émet                          ──> clientes

    melonDS suspend son émulation dans RecvReplies() jusqu'à ce que toutes
    les clientes aient répondu ou que l'ESP32 ait clos la fenêtre : le délai
    de l'USB est absorbé par l'émulateur, pas par les vraies DS.
*/

#include "ESP32MP.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "Platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace
{
// Miroir de common/linkproto.h (ds-pc-link). À garder synchronisé.
constexpr u8 SER_CMD_PING = 0x01;
constexpr u8 SER_CMD_SET_MODE = 0x03;
constexpr u8 SER_CMD_MP_TX = 0x08;
constexpr u8 SER_CMD_MP_CONFIG = 0x09;
constexpr u8 SER_EVT_PONG = 0x80;
constexpr u8 SER_EVT_RX_RAW = 0x82;
constexpr u8 SER_EVT_LOG = 0x83;
constexpr u8 SER_EVT_MP_REPLY = 0x85;
constexpr u8 SER_EVT_MP_WINDOW = 0x86;
constexpr u8 MODE_MP_HOST = 3;
constexpr u8 MP_TX_CMD = 0x01;
constexpr u8 RATE_2M = 0x14;
constexpr int DS_HW_HDR_LEN = 12;
constexpr int kBaud = 921600;
constexpr size_t kMaxSerFrame = 1700;
constexpr size_t kMaxRXQueue = 256;
constexpr size_t kReplySlot = 1024;          // MPClientReplies : 1024 o par cliente

u16 CRC16(const u8* d, size_t n)
{
    u16 crc = 0xFFFF;
    while (n--)
    {
        crc ^= (u16)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    }
    return crc;
}

size_t COBSEncode(const u8* in, size_t len, u8* out)
{
    size_t r = 0, w = 1, code_idx = 0;
    u8 code = 1;
    while (r < len)
    {
        if (in[r] == 0) { out[code_idx] = code; code = 1; code_idx = w++; r++; }
        else
        {
            out[w++] = in[r++]; code++;
            if (code == 0xFF) { out[code_idx] = code; code = 1; code_idx = w++; }
        }
    }
    out[code_idx] = code;
    return w;
}

size_t COBSDecode(const u8* in, size_t len, u8* out)
{
    size_t r = 0, w = 0;
    while (r < len)
    {
        u8 code = in[r++];
        if (code == 0) return 0;
        for (u8 i = 1; i < code; i++)
        {
            if (r >= len) return 0;
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) out[w++] = 0;
    }
    return w;
}

u64 MACKey(const u8* m)
{
    u64 k = 0;
    for (int i = 0; i < 6; i++) k = (k << 8) | m[i];
    return k;
}
}

// ---------------------------------------------------------------------------

bool ESP32MP::Configured() noexcept
{
    const char* p = std::getenv("MELONDS_ESP32_PORT");
    return p && p[0];
}

ESP32MP::ESP32MP() noexcept
{
    const char* name = std::getenv("MELONDS_ESP32_PORT");
    if (!name || !name[0] || !OpenPort(name))
    {
        Log(LogLevel::Error, "ESP32MP: impossible d'ouvrir le port %s\n", name ? name : "(vide)");
        return;
    }
    Log(LogLevel::Info, "ESP32MP: port %s ouvert\n", name);

    Reader = std::thread([this] { ReaderLoop(); });

    u8 mode = MODE_MP_HOST;
    SendMsg(SER_CMD_SET_MODE, &mode, 1);

    u16 win = 0;
    if (const char* w = std::getenv("MELONDS_ESP32_WINDOW_US")) win = (u16)std::atoi(w);
    if (win >= 200)
    {
        u8 p[2] = { (u8)(win & 0xFF), (u8)(win >> 8) };
        SendMsg(SER_CMD_MP_CONFIG, p, 2);
    }
    SendMsg(SER_CMD_PING, nullptr, 0);
}

ESP32MP::~ESP32MP() noexcept
{
    Stop = true;
    if (Reader.joinable()) Reader.join();
    ClosePort();
}

void ESP32MP::Begin(int inst)
{
    if (HostInst < 0)
    {
        HostInst = inst;
        Log(LogLevel::Info, "ESP32MP: instance %d reliée à l'ESP32 (elle doit être l'hôte)\n", inst);
    }
    else if (inst != HostInst)
    {
        Log(LogLevel::Warn, "ESP32MP: une seule instance est gérée, l'instance %d est ignorée\n", inst);
    }
}

void ESP32MP::End(int inst)
{
    if (inst == HostInst) HostInst = -1;
}

// ---------------------------------------------------------------------------
// Émission (melonDS -> ESP32 -> ondes)
// ---------------------------------------------------------------------------

void ESP32MP::NoteHostFrame(const u8* data, int len, u64 timestamp) noexcept
{
    if (len < DS_HW_HDR_LEN + 24) return;
    LastTimestamp = timestamp ? timestamp : 1;
    Channel = data[9];

    // Réponse d'association (ou de réassociation) acceptée : elle donne l'AID
    // de la cliente, qui servira à ranger ses réponses aux CMD.
    const u8* f = data + DS_HW_HDR_LEN;
    u8 subtype = f[0] & 0xFC;
    if ((subtype == 0x10 || subtype == 0x30) && len >= DS_HW_HDR_LEN + 24 + 6)
    {
        u16 status = (u16)(f[26] | (f[27] << 8));
        u16 aid = (u16)((f[28] | (f[29] << 8)) & 0x3FFF);
        if (status == 0 && aid >= 1 && aid <= 15)
        {
            std::lock_guard<std::mutex> g(Lock);
            MACToAID[MACKey(f + 4)] = aid;
            Log(LogLevel::Info, "ESP32MP: cliente %02X:%02X:%02X:%02X:%02X:%02X associée, AID %d\n",
                f[4], f[5], f[6], f[7], f[8], f[9], aid);
        }
    }
    else if (subtype == 0xA0 || subtype == 0xC0)     // désassociation / désauthentification
    {
        std::lock_guard<std::mutex> g(Lock);
        MACToAID.erase(MACKey(f + 4));
    }
}

int ESP32MP::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    if (inst != HostInst) return len;
    NoteHostFrame(data, len, timestamp);
    SendMPTx(0, 0, data, len);
    return len;
}

int ESP32MP::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    if (inst != HostInst) return len;
    NoteHostFrame(data, len, timestamp);
    u8 id;
    {
        std::lock_guard<std::mutex> g(Lock);
        id = ++CmdID;
        Replies.clear();
        ReplyMask = 0;
        WindowDone = false;
    }
    SendMPTx(MP_TX_CMD, id, data, len);
    return len;
}

int ESP32MP::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    if (inst != HostInst) return len;
    NoteHostFrame(data, len, timestamp);
    SendMPTx(0, 0, data, len);
    return len;
}

int ESP32MP::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    // melonDS client d'une vraie DS hôte : non géré par cette version.
    return len;
}

void ESP32MP::SendMPTx(u8 flags, u8 id, const u8* data, int len) noexcept
{
    if (len < DS_HW_HDR_LEN + 24 || (size_t)len + 2 > kMaxSerFrame) return;
    std::vector<u8> p(2 + (size_t)len);
    p[0] = flags;
    p[1] = id;
    std::memcpy(p.data() + 2, data, (size_t)len);
    SendMsg(SER_CMD_MP_TX, p.data(), p.size());
}

// ---------------------------------------------------------------------------
// Réception (ondes -> ESP32 -> melonDS)
// ---------------------------------------------------------------------------

std::vector<u8> ESP32MP::MakeDSBuffer(u8 rate, const u8* frame, size_t len) const
{
    // Tampon matériel attendu par Wifi.cpp : en-tête de 12 octets (débit à 8,
    // canal à 9, longueur FCS compris à 10), trame, puis 4 octets de FCS.
    // Le canal est celui de l'hôte émulé, sinon CheckRX() rejette la trame.
    std::vector<u8> b(DS_HW_HDR_LEN + len + 4, 0);
    b[8] = rate;
    b[9] = Channel;
    u16 flen = (u16)(len + 4);
    b[10] = (u8)(flen & 0xFF);
    b[11] = (u8)(flen >> 8);
    std::memcpy(b.data() + DS_HW_HDR_LEN, frame, len);
    return b;
}

int ESP32MP::RecvPacket(int inst, u8* data, u64* timestamp)
{
    if (inst != HostInst) return 0;
    std::vector<u8> b;
    {
        std::lock_guard<std::mutex> g(Lock);
        if (RXQueue.empty()) return 0;
        b = std::move(RXQueue.front());
        RXQueue.pop_front();
    }
    std::memcpy(data, b.data(), b.size());
    // Un horodatage non nul est exigé pour traiter authentification et
    // association : on donne celui de l'hôte, la trame est « reçue maintenant ».
    if (timestamp) *timestamp = LastTimestamp;
    return (int)b.size();
}

int ESP32MP::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // Réservé au rôle de cliente, non géré : aucun hôte.
    return 0;
}

u16 ESP32MP::RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask)
{
    if (inst != HostInst) return 0;
    std::unique_lock<std::mutex> g(Lock);
    ReplyCV.wait_for(g, std::chrono::milliseconds(RecvTimeout), [&] {
        return WindowDone || (ReplyMask & aidmask) == aidmask;
    });

    u16 ret = 0;
    for (const Reply& r : Replies)
    {
        u16 bit = (u16)(1 << r.AID);
        if (!(aidmask & bit) || (ret & bit) || r.Buf.size() > kReplySlot) continue;
        std::memcpy(&data[(r.AID - 1) * kReplySlot], r.Buf.data(), r.Buf.size());
        ret |= bit;
    }
    return ret;
}

void ESP32MP::HandleMsg(const u8* m, size_t n) noexcept
{
    u8 type = m[0];
    const u8* p = m + 1;
    size_t pl = n - 1;

    switch (type)
    {
    case SER_EVT_RX_RAW:            // [rssi][canal][ts u32][trame]
    {
        if (pl < 6 + 24) break;
        std::vector<u8> b = MakeDSBuffer(RATE_2M, p + 6, pl - 6);
        std::lock_guard<std::mutex> g(Lock);
        if (RXQueue.size() >= kMaxRXQueue) RXQueue.pop_front();
        RXQueue.push_back(std::move(b));
        break;
    }
    case SER_EVT_MP_REPLY:          // [id][rssi][rate][ts u32][trame]
    {
        if (pl < 7 + 24) break;
        const u8* f = p + 7;
        std::lock_guard<std::mutex> g(Lock);
        if (p[0] != CmdID) break;                     // réponse à une ancienne CMD
        auto it = MACToAID.find(MACKey(f + 10));
        if (it == MACToAID.end()) break;               // cliente inconnue
        Replies.push_back({ it->second, MakeDSBuffer(p[2], f, pl - 7) });
        ReplyMask |= (u16)(1 << it->second);
        ReplyCV.notify_all();
        break;
    }
    case SER_EVT_MP_WINDOW:         // [id][nb][tardives u32]
    {
        if (pl < 6) break;
        u32 late;
        std::memcpy(&late, p + 2, 4);
        {
            std::lock_guard<std::mutex> g(Lock);
            if (p[0] == CmdID)
            {
                WindowDone = true;
                ReplyCV.notify_all();
            }
        }
        if (late != LastLate)
        {
            if (late > LastLate)
                Log(LogLevel::Warn, "ESP32MP: %u réponse(s) arrivée(s) hors fenêtre au total\n", late);
            LastLate = late;
        }
        break;
    }
    case SER_EVT_LOG:
        Log(LogLevel::Info, "ESP32: %.*s\n", (int)pl, (const char*)p);
        break;
    case SER_EVT_PONG:
        if (pl >= 7)
            Log(LogLevel::Info, "ESP32MP: ESP32 prêt, firmware v%d, MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
        break;
    default:
        break;
    }
}

void ESP32MP::ReaderLoop() noexcept
{
    std::vector<u8> acc;
    acc.reserve(kMaxSerFrame * 2);
    u8 chunk[512];
    u8 dec[kMaxSerFrame * 2];
    bool overflow = false;

    while (!Stop)
    {
        int r = ReadPort(chunk, sizeof chunk);
        for (int i = 0; i < r; i++)
        {
            if (chunk[i] != 0)
            {
                if (acc.size() < sizeof dec) acc.push_back(chunk[i]);
                else overflow = true;
                continue;
            }
            if (!overflow && !acc.empty())
            {
                size_t d = COBSDecode(acc.data(), acc.size(), dec);
                if (d >= 3)
                {
                    u16 crc = (u16)(dec[d - 2] | (dec[d - 1] << 8));
                    if (crc == CRC16(dec, d - 2)) HandleMsg(dec, d - 2);
                }
            }
            acc.clear();
            overflow = false;
        }
    }
}

void ESP32MP::SendMsg(u8 type, const u8* payload, size_t len) noexcept
{
    if (len + 3 > kMaxSerFrame) return;
    u8 raw[kMaxSerFrame + 8];
    u8 enc[kMaxSerFrame + kMaxSerFrame / 254 + 16];
    size_t n = 0;
    raw[n++] = type;
    if (len) { std::memcpy(raw + n, payload, len); n += len; }
    u16 crc = CRC16(raw, n);
    raw[n++] = (u8)(crc & 0xFF);
    raw[n++] = (u8)(crc >> 8);
    size_t e = COBSEncode(raw, n, enc);
    enc[e++] = 0x00;
    std::lock_guard<std::mutex> g(WriteLock);
    WritePort(enc, e);
}

// ---------------------------------------------------------------------------
// Port série
// ---------------------------------------------------------------------------

#ifdef _WIN32

bool ESP32MP::OpenPort(const std::string& name) noexcept
{
    std::string path = "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb {};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = kBaud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    // DTR et RTS au repos : sur les cartes ESP32, ces lignes pilotent le
    // reset et le mode programmation.
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS to {};
    to.ReadIntervalTimeout = MAXDWORD;          // rend la main dès qu'il y a des octets…
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 20;           // … ou au bout de 20 ms
    to.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    Port = h;
    return true;
}

void ESP32MP::ClosePort() noexcept
{
    if (Port) { CloseHandle((HANDLE)Port); Port = nullptr; }
}

bool ESP32MP::WritePort(const u8* data, size_t len) noexcept
{
    if (!Port) return false;
    DWORD w = 0;
    return WriteFile((HANDLE)Port, data, (DWORD)len, &w, nullptr) && w == len;
}

int ESP32MP::ReadPort(u8* data, size_t len) noexcept
{
    if (!Port) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return 0; }
    DWORD r = 0;
    if (!ReadFile((HANDLE)Port, data, (DWORD)len, &r, nullptr)) return 0;
    return (int)r;
}

#else

bool ESP32MP::OpenPort(const std::string& name) noexcept
{
    int fd = open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;
    termios t {};
    if (tcgetattr(fd, &t) != 0) { close(fd); return false; }
    cfmakeraw(&t);
#ifdef B921600
    cfsetispeed(&t, B921600);
    cfsetospeed(&t, B921600);
#endif
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    if (tcsetattr(fd, TCSANOW, &t) != 0) { close(fd); return false; }
    tcflush(fd, TCIOFLUSH);
    Port = fd;
    return true;
}

void ESP32MP::ClosePort() noexcept
{
    if (Port >= 0) { close(Port); Port = -1; }
}

bool ESP32MP::WritePort(const u8* data, size_t len) noexcept
{
    size_t off = 0;
    while (Port >= 0 && off < len)
    {
        ssize_t w = write(Port, data + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        pollfd pfd { Port, POLLOUT, 0 };
        if (poll(&pfd, 1, 200) <= 0) return false;
    }
    return off == len;
}

int ESP32MP::ReadPort(u8* data, size_t len) noexcept
{
    if (Port < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return 0; }
    pollfd pfd { Port, POLLIN, 0 };
    if (poll(&pfd, 1, 20) <= 0) return 0;
    ssize_t r = read(Port, data, len);
    return r > 0 ? (int)r : 0;
}

#endif

}

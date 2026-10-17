/*
    O11 Session - uitext.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  What the editor's controls show and take, without JUCE (so the unit test
  can check it): the BPM box (dragged or typed), the interval choices, the
  volume boxes' dB, the meters' scale, the beat box, the players' and the room
  panel's texts (what happened in the room, and how long ago), and the host
  bar's address and port texts.

*/

#ifndef _O11_UITEXT_H_
#define _O11_UITEXT_H_

#include <string>

namespace o11 {
namespace uitext {

constexpr int BPM_MIN = 40, BPM_MAX = 240;           // what the BPM box votes for
constexpr int BPI_CHOICES[] = { 2, 4, 8, 12, 16 }; // Leo's list; a typed /bpi can still set up to 64
constexpr int NUM_BPI_CHOICES = 5;
constexpr double PIXELS_PER_BPM = 2.0;               // dragging the BPM box up or down (4 times as far with Shift)
// the level meters show -60 dB to 0 dB: green, amber from -12 dB, red from -3 dB
constexpr float METER_FLOOR_DB = -60.0f, METER_AMBER = 0.8f, METER_RED = 0.95f;
constexpr double JOINED_SECONDS = 30.0;              // a player's row says "joined" this long
constexpr double DROPOUT_SECONDS = 60.0;             // and how many dropouts it had, this long after one

// the BPM box dragged dy pixels up (down when negative) from start, within BPM_MIN..BPM_MAX
int dragBpm(int start, double dy, bool fine);
// what was typed in the BPM box: a number ("125", "125.4"), rounded, within BPM_MIN..BPM_MAX;
// false when it isn't a number
bool parseBpm(const std::string &text, int *bpm);
// the room's tempo in the BPM box: "120.00", or a dash when there is none
std::string bpmBoxText(double bpm);
// a gain (1 = 0 dB) as a volume box shows it: "-12.0 dB", "0.0 dB", "+6.0 dB", "-inf dB"
std::string dbText(float gain);
// a peak level (linear) on a meter: 0 at METER_FLOOR_DB and below, 1 at 0 dB and above
float meterFraction(float peak);
// the beat box: "3 / 16"; a dash for the beat when no interval plays ("– / 16"), and for both without a room
std::string beatText(int pos, int length, int bpi);
// an Opus bitrate in the bitrate box: "128k"
std::string kbpsText(int kbps);
// an interval's length in the Interval box: "16 beats"
std::string beatsText(int beats);
// the room panel's count, you included: "just you", "4 players"
std::string playersText(int players);
// a player's row, seconds after they joined: "just joined", "joined 12 s ago"
std::string joinedText(double seconds);
// a player's gaps in their audio: "1 dropout", "3 dropouts"
std::string dropoutsText(long n);
// Live's jitter buffer in the Buffer box: "20 ms"
std::string msText(int ms);
// the Mode box: "Live 28 ms" while the room plays Live ("Live" until the delay is known), else
// "Intervals"
std::string liveModeText(bool live, int heardMs);
// why the room plays intervals while its Live switch is on (the Snapshot's liveReason: 1 a
// connection's dropouts, 2 a client that can't play Live, 3 someone joining) and whose:
// "Intervals: Anna's connection", "Intervals: your connection", "Intervals: Carol can't play
// Live", "Intervals while Anna joins"; "" for no reason
std::string liveAdviceText(int reason, const std::string &who, int others);
// how long ago something happened, in the room panel: "now", "12 s", "2 min", "1 h"
std::string ageText(double seconds);
// a server's line with the players' addresses taken out: "anna@203.0.113.9 set ..." is "anna set ..."
std::string withoutAddresses(const std::string &text);

// The room panel's list of what happened in the room, from the lines the engine keeps (its
// chat, with the plug-in's own notes and the server's messages): who came and went, what
// changed (tempo, interval, codec, bitrate), and what the server says. The kinds are the
// engine's (ChatLine): the plug-in's notes, the server's messages, the built-in server's log,
// joins, leaves, what players say (only their votes are shown), the engine's Live lines (the
// room went Live, or back on intervals), anything else (errors the editor shows elsewhere).
enum class LineKind { Note, Server, HostLog, Join, Leave, Chat, Live, Other };
struct RoomEvent
{
  enum Tone { Plain, Change, Warn }; // grey, purple, amber
  bool shown = false;
  Tone tone = Plain;
  std::string text;
};
// who: the display name of who joined, left or said it; hosting: you host the room; voter: who
// cast the vote a voting system line is about ("You" for you), "" when not known
RoomEvent roomEvent(LineKind kind, const std::string &who, const std::string &text, bool hosting,
                    const std::string &voter = "");

// A player's vote as the server passes it on ("!vote bpm 100"): BPM or interval, and the value.
bool parseVote(const std::string &text, bool *bpm, int *value);
// Whether a voting system line is about that vote: the room set to its value, or its value leading.
bool voteMatches(bool bpm, int value, const std::string &serverText);

// The host bar while hosting, from what the built-in server knows (the Snapshot's host fields).
struct HostInfo
{
  int port = 0;
  std::string publicAddress;   // the router's word (the port mapper): host:port
  std::string lookedUpAddress; // a "what's my IP" service's, with the port
  std::string localAddress;    // in the local network, with the port
  std::string portmapNote;     // the last thing the port mapper said, when it failed
  bool portmapPending = false, lookupPending = false;
  bool joinedFromInternet = false;
};
struct HostBar
{
  // The address box: the address people connect to. The router's when it forwarded the port,
  // else the looked-up public address (the port may be forwarded by hand), else the LAN address.
  std::string addressKey;  // "WAN" or "LAN"
  std::string address;     // empty: the placeholder shows
  std::string placeholder; // "looking it up..." while asking, else "not known"
  std::string addressTip;
  // the port's state on the right; Blocked: the router forwards it, but its own address is in
  // the internet provider's shared range (carrier-grade NAT), so only your network can join
  enum Port { Open, Asking, Forward, Blocked };
  Port port = Forward;
  std::string portText;    // "Port open", "Asking the router...", "Forward port 2049", "Only your network can join"
  std::string portTip;     // the whole story, with the router's note when it failed
};
HostBar hostBar(const HostInfo &h);
// an address as people type it: without the port when it is the usual 2049 ("203.0.113.7")
std::string roomAddress(const std::string &address);

} // namespace uitext
} // namespace o11

#endif

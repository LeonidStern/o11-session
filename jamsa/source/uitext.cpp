/*
    Jamsa - uitext.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "uitext.h"

#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include <algorithm>

namespace jamsa {
namespace uitext {

static int clampBpm(long v) { return (int)std::min<long>(BPM_MAX, std::max<long>(BPM_MIN, v)); }

int dragBpm(int start, double dy, bool fine)
{
  const double steps = dy / (PIXELS_PER_BPM * (fine ? 4.0 : 1.0));
  return clampBpm(start + (long)(steps < 0 ? ceil(steps) : floor(steps)));
}

bool parseBpm(const std::string &text, int *bpm)
{
  // digits, maybe a decimal point or comma and more digits, maybe "bpm"; read without the
  // locale's help (a host may have set one that writes 95,5, and either is fine to type)
  size_t i = text.find_first_not_of(" \t");
  if (i == std::string::npos) return false;
  double v = 0;
  int digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') { v = std::min(1e9, v * 10 + (text[i++] - '0')); digits++; }
  if (i < text.size() && (text[i] == '.' || text[i] == ','))
  {
    i++;
    for (double f = 0.1; i < text.size() && text[i] >= '0' && text[i] <= '9'; i++, f /= 10) { v += (text[i] - '0') * f; digits++; }
  }
  if (!digits) return false;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
  std::string rest = text.substr(i);
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
  for (char &c : rest) c = (char)tolower((unsigned char)c);
  if (!rest.empty() && rest != "bpm") return false;
  if (bpm) *bpm = clampBpm(lround(v));
  return true;
}

std::string bpmBoxText(double bpm)
{
  if (!(bpm > 0) || bpm > 1e6) return "\xe2\x80\x94"; // an em dash
  // "120.00" whatever the locale says
  const long hundredths = lround(bpm * 100);
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld.%02ld", hundredths / 100, hundredths % 100);
  return buf;
}

std::string dbText(float gain)
{
  if (!(gain > 0.00001f)) return "-inf dB";
  const long tenths = lround(20.0 * log10(gain) * 10);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s%ld.%ld dB", tenths > 0 ? "+" : tenths < 0 ? "-" : "", labs(tenths) / 10, labs(tenths) % 10);
  return buf;
}

int litSegments(float peak)
{
  // dB where each segment lights: five green, then two yellow
  static const float th[METER_SEGMENTS] = { -48, -36, -24, -16, -10, -5, -1 };
  if (!(peak > 0)) return 0;
  const float db = 20.0f * log10f(peak);
  int n = 0;
  while (n < METER_SEGMENTS && db >= th[n]) n++;
  return n;
}

std::string beatText(int pos, int length, int bpi)
{
  if (bpi <= 0) return "\xe2\x80\x93 / \xe2\x80\x93"; // en dashes
  if (length <= 0) return "\xe2\x80\x93 / " + std::to_string(bpi);
  const int beat = std::min(bpi - 1, std::max(0, (int)((double)pos / length * bpi)));
  return std::to_string(beat + 1) + " / " + std::to_string(bpi);
}

std::string kbpsText(int kbps)
{
  return std::to_string(kbps) + "k";
}

} // namespace uitext
} // namespace jamsa

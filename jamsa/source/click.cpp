/*
    Jamsa - click.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "click.h"

#include <math.h>

namespace jamsa {

static const double PI = 3.14159265358979323846;

int Click::length(double sampleRate)
{
  return sampleRate > 0 ? (int)ceil(LENGTH * sampleRate) : 0;
}

double Click::sample(bool accent, int n, double sampleRate)
{
  if (n < 0 || sampleRate <= 0) return 0;
  const double t = n / sampleRate;
  if (t >= LENGTH) return 0;
  // the attack: a raised cosine from 0 to 1
  const double attack = t < ATTACK ? 0.5 * (1.0 - cos(PI * t / ATTACK)) : 1.0;
  // the decay: exponential, to -60 dB at SIXTY_DB, less what is left at LENGTH, so it ends at 0
  const double tau = (SIXTY_DB - ATTACK) / log(1000.0);
  const double left = exp(-(LENGTH - ATTACK) / tau);
  const double decay = t <= ATTACK ? 1.0 : (exp(-(t - ATTACK) / tau) - left) / (1.0 - left);
  const double hz = accent ? ACCENT_HZ : BEAT_HZ, level = accent ? 1.0 : BEAT_LEVEL;
  return level * attack * decay * sin(2.0 * PI * hz * t);
}

void Click::setSampleRate(double sampleRate)
{
  if (sampleRate <= 0) return;
  m_sampleRate = sampleRate;
  m_length = length(sampleRate);
  m_n = -1;
}

void Click::reset()
{
  m_n = -1;
}

// where beat k starts in an interval of len samples and bpi beats
static long long beat_start(int k, int len, int bpi)
{
  return ((long long)k * len * 2 + bpi) / ((long long)bpi * 2); // k * len / bpi, rounded
}

void Click::render(float *left, float *right, int frames, int start, int len, int bpi, float volume)
{
  if (!left || frames <= 0) return;
  // the next beat at or after `start`
  int k = 0;
  long long next = -1;
  if (len > 0 && bpi > 0)
  {
    if (start > 0)
    {
      k = (int)((long long)start * bpi / len);
      while (k > 0 && beat_start(k - 1, len, bpi) >= start) k--;
      while (k < bpi && beat_start(k, len, bpi) < start) k++;
    }
    if (k < bpi) next = beat_start(k, len, bpi);
  }

  for (int i = 0; i < frames; i ++)
  {
    if (start + i == next)
    {
      m_n = 0;
      m_accent = k == 0;
      next = ++k < bpi ? beat_start(k, len, bpi) : -1;
    }
    if (m_n < 0) continue;
    if (volume > 0)
    {
      const float v = (float)(sample(m_accent, m_n, m_sampleRate) * volume);
      left[i] += v;
      if (right) right[i] += v;
    }
    if (++m_n >= m_length) m_n = -1;
  }
}

} // namespace jamsa

/*
    Jamsa - processor.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "processor.h"

#include "editor.h"

namespace jamsa {

static const size_t MAX_STATE_SIZE = 1 << 20;

juce::AudioProcessor::BusesProperties JamsaProcessor::buses()
{
  juce::AudioProcessor::BusesProperties b;
  b.addBus(true, "Input", juce::AudioChannelSet::stereo(), true);
  b.addBus(false, "Output", juce::AudioChannelSet::stereo(), true);
  // a stereo output for each player alone (Engine::PlayerOuts), for recording each on a track
  for (int i = 0; i < Engine::NUM_PLAYER_OUTS; i ++)
    b.addBus(false, "Player " + juce::String(i + 1), juce::AudioChannelSet::stereo(), true);
  return b;
}

JamsaProcessor::JamsaProcessor()
  : juce::AudioProcessor(buses()), m_engine(new Engine)
{
  m_engine->setHostName(juce::PluginHostType().getHostDescription());
  m_engine->start();
}

JamsaProcessor::~JamsaProcessor()
{
  m_engine->stop();
}

void JamsaProcessor::prepareToPlay(double sampleRate, int maxBlockSize)
{
  m_engine->setupProcessing(sampleRate, maxBlockSize);
}

bool JamsaProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
  // a mono or stereo input, a stereo main output; the player outputs stereo or off
  const juce::AudioChannelSet in = layouts.getMainInputChannelSet();
  if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo() && !in.isDisabled()) return false;
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
  for (int i = 1; i < layouts.outputBuses.size(); i ++)
  {
    const juce::AudioChannelSet &set = layouts.outputBuses.getReference(i);
    if (set != juce::AudioChannelSet::stereo() && !set.isDisabled()) return false;
  }
  return true;
}

HostPosition JamsaProcessor::hostPosition(const juce::AudioPlayHead::PositionInfo &info)
{
  HostPosition p;
  p.playing = info.getIsPlaying();
  if (const auto tempo = info.getBpm()) p.tempo = *tempo > 0 ? *tempo : 0;
  if (const auto ppq = info.getPpqPosition())
  {
    p.hasPosition = true;
    p.ppq = *ppq;
  }
  if (const auto loop = info.getLoopPoints())
  {
    p.cycleActive = info.getIsLooping();
    p.cycleStart = loop->ppqStart;
    p.cycleEnd = loop->ppqEnd;
  }
  if (const auto sig = info.getTimeSignature())
  {
    p.timeSigNum = sig->numerator;
    p.timeSigDen = sig->denominator;
  }
  return p;
}

void JamsaProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &)
{
  juce::ScopedNoDenormals noDenormals;
  const int frames = buffer.getNumSamples();
  if (frames <= 0) return;

  // the input, read before the main output is written (they may share buffers: the engine
  // copies its input first, piece by piece, and writes the output of each piece after)
  juce::AudioBuffer<float> inBus = getBusBuffer(buffer, true, 0);
  juce::AudioBuffer<float> outBus = getBusBuffer(buffer, false, 0);
  if (outBus.getNumChannels() < 1) return;
  float *in[2] = { nullptr, nullptr };
  int numIn = 0;
  if (getBus(true, 0)->isEnabled())
    for (int c = 0; c < std::min(2, inBus.getNumChannels()); c ++) in[numIn++] = inBus.getWritePointer(c);
  float *out[2] = { outBus.getWritePointer(0), outBus.getNumChannels() > 1 ? outBus.getWritePointer(1) : nullptr };

  Engine::HostTiming timing;
  timing.offline = isNonRealtime();
  if (juce::AudioPlayHead *head = getPlayHead())
    if (const auto info = head->getPosition())
    {
      timing.transport = true;
      timing.position = hostPosition(*info);
    }

  Engine::PlayerOuts players;
  float *playerChannels[Engine::NUM_PLAYER_OUTS][2];
  for (int b = 1; b < getBusCount(false) && b <= Engine::NUM_PLAYER_OUTS; b ++)
  {
    if (!getBus(false, b)->isEnabled()) continue;
    juce::AudioBuffer<float> bus = getBusBuffer(buffer, false, b);
    const int channels = std::min(2, bus.getNumChannels());
    if (channels < 1) continue;
    for (int c = 0; c < channels; c ++) playerChannels[b - 1][c] = bus.getWritePointer(c);
    players.bus[b - 1] = playerChannels[b - 1];
    players.channels[b - 1] = channels;
  }

  m_engine->process(in, numIn, out, outBus.getNumChannels() > 1 ? 2 : 1, frames, &timing, &players);
}

juce::AudioProcessorEditor *JamsaProcessor::createEditor()
{
  return new JamsaEditor(*this);
}

// The state is text (see Settings::serialize), the same format as the plug-in before this one
void JamsaProcessor::getStateInformation(juce::MemoryBlock &dest)
{
  const std::string data = m_engine->getSettings().serialize();
  dest.replaceAll(data.data(), data.size());
}

void JamsaProcessor::setStateInformation(const void *data, int size)
{
  if (!data || size <= 0 || (size_t)size > MAX_STATE_SIZE) return;
  Settings s;
  if (s.deserialize(std::string((const char *)data, (size_t)size))) m_engine->setSettings(s);
}

} // namespace jamsa

// JUCE's entry point
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
  return new jamsa::JamsaProcessor;
}

// tw2de (2026-10-21): the shader variants the game used in play, kept in a text file next to the NRO, which the
// next start's pre-pass compiles in full (the game's own run-time compiles are a fast pass). A list recorded over a
// whole play-through can ship with a release (tw2de_variants_release.txt): the pre-pass compiles those too, so a new
// install has them before they are first drawn. No shader is in it: only which stored shader and which constants.
//
// One variant a line:
//   <translator tag> <key> <shader hash> <v|p> <id>=<value>;<id>=<value>;...
// all numbers hexadecimal except the constants' ids and values (decimal, as plume writes them). The key is the DKSH
// cache's (FNV-1a 64 of the variant's text) for the translation the tag names; a line of another translation has its
// key computed again by the pre-pass. A variant is the same one when its shader hash, stage and constants are.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renut_engine/gpu/nx_shaders/nx_lock.h"

namespace renut::nxs {

struct VariantEntry {
  uint64_t tag = 0;   // TranslatorTag() of the translation the key is for
  uint64_t key = 0;   // the DKSH cache's key
  uint64_t hash = 0;  // the shader's (the store's)
  bool pixel = false;
  std::vector<std::pair<uint32_t, uint32_t>> values;  // the constants that differ from the shader's defaults, by id

  std::string Identity() const;  // hash, stage and constants
  std::string Line() const;
  static bool Parse(const std::string &line, VariantEntry &out);
};

// TW2 (2026-11-16; the user: "i wanted text file to contain only standard ones, adding motion vector twin sounds
// like waste of space"): a variant's motion-vector form (FSR 3.1) is the same constants with specialization constant
// 914's bit 2 set (xenos_glsl.cpp). No list has such a line: the lists hold the plain forms, and with FSR 3.1 on
// the pre-pass compiles the twin of every listed variant and of every default variant. A twin's DKSH key is not the
// hash of its text, as the other variants' is: it is made from its plain form's key (MotionTwinKey), so neither
// the pre-pass nor the game keeps it anywhere or reads the shader's text for it.
constexpr uint32_t kVariantWordFx = 914, kVariantMotionBit = 4;
bool MotionVariant(const VariantEntry &e);
// A motion-vector variant made its plain form: the bit cleared, the constant gone when nothing is left of it, no
// tag and no key (they were the twin's). False: it was a plain one and is as it was.
bool StripMotion(VariantEntry &e);
uint64_t MotionTwinKey(uint64_t plain_key);

// The variant's text, as plume's loadVariant makes it: the shader's GLSL with a define for each constant whose value
// differs from the shader's default, in ascending id order, after the first line (#version).
std::string VariantSource(const std::string &glsl, const std::vector<std::pair<uint32_t, uint32_t>> &values);

// The file's lines (unreadable or missing: none).
std::vector<VariantEntry> LoadVariantList(const std::string &path);
// The whole file written again (the pre-pass, with the keys of the current translation).
bool SaveVariantList(const std::string &path, const std::vector<VariantEntry> &entries);

// The recorder the renderer keeps during play: lines appended (and flushed) once per variant not in the file yet.
class VariantRecorder {
 public:
  static VariantRecorder &Get();
  void Open(const std::string &path);
  void Record(const VariantEntry &entry);
  size_t Recorded() const { return recorded_; }
  // tw2de (2026-10-23): with a deferred recorder Record only keeps the line and calls 'kick' (the renderer's card
  // writer thread), which writes them with Flush: no card write on the thread that compiled (the game waits for it).
  void SetDeferred(void (*kick)()) { kick_ = kick; }
  void Flush();
  // tw2de (2026-10-24): the DKSH keys of the variants in these lists made by the translation 'tag', and of those
  // recorded since (plume's Deko3DVariantKeyLookup: a shader made from its metadata needs its text only for a
  // variant no list knows).
  void LoadKeys(const std::vector<std::string> &paths, uint64_t tag);
  // TW2 (2026-11-16): a motion-vector twin's key is made from its plain form's: a listed variant's, or
  // 'default_key' (the shader's default variant's) when the twin is the default's.
  bool KeyOf(uint64_t hash, bool pixel, const std::vector<std::pair<uint32_t, uint32_t>> &values,
             uint64_t default_key, uint64_t &key);

 private:
  NxMutex mutex_;
  std::unordered_map<std::string, uint64_t> keys_;  // identity -> key
  uint64_t keys_tag_ = 0;
  void (*kick_)() = nullptr;
  std::string pending_;
  std::string path_;
  std::vector<std::string> known_;  // identities, sorted
  size_t recorded_ = 0;
  bool open_ = false;
};

}  // namespace renut::nxs
